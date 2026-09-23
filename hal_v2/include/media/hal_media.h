/**
 * @file hal_media.h
 * @brief HAL Media - Unified media pipeline management.
 *
 * The media component owns the full capture-to-encode pipeline on platforms
 * that require unified management (e.g. Hailo-15 MediaLibrary).  It provides:
 *   - Pipeline lifecycle (init / start / stop / deinit)
 *   - Profile management (list / switch / get current)
 *   - Image configuration (rotation, flip, zoom, stabilization, privacy mask)
 *   - Factory for video and codec contexts via get_video_list / get_codec_list
 *   - Optional automatic frontend-to-encoder forwarding (set_encoder_auto_feed)
 *
 * Video and codec contexts obtained from this component have type
 * HAL_VIDEO_TYPE_FROM_MEDIA / HAL_CODEC_TYPE_FROM_MEDIA.  They must NOT
 * be initialized or deinitialized directly; their lifecycle is tied to the
 * media pipeline.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../common/hal_common.h"
#include "../common/hal_buffer.h"
#include "hal_video.h"
#include "hal_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------
 * Image rotation / flip enumerations
 * -------------------------------------------------------------------- */

/** Clockwise rotation angle applied to the captured image. */
typedef enum {
    HAL_ROTATION_ANGLE_0   = 0,     /* no rotation */
    HAL_ROTATION_ANGLE_90  = 1,     /* 90 degrees clockwise */
    HAL_ROTATION_ANGLE_180 = 2,     /* 180 degrees */
    HAL_ROTATION_ANGLE_270 = 3,     /* 270 degrees clockwise (= 90 CCW) */
} HalRotationAngle;

/** Mirror / flip direction. */
typedef enum {
    HAL_FLIP_DIRECTION_NONE       = 0,  /* no flip */
    HAL_FLIP_DIRECTION_HORIZONTAL = 1,  /* left-right mirror */
    HAL_FLIP_DIRECTION_VERTICAL   = 2,  /* top-bottom mirror */
    HAL_FLIP_DIRECTION_BOTH       = 3,  /* both axes (= 180 rotation) */
} HalFlipDirection;

/* --------------------------------------------------------------------
 * Config-field patching (set_config_field)
 * --------------------------------------------------------------------
 * Value type of a single profile field modified at runtime via
 * set_config_field(). The value is always passed as a string and
 * converted by the implementation according to this type. */
typedef enum {
    HAL_CONFIG_FIELD_BOOL   = 0,  /* "true"/"false"/"1"/"0" */
    HAL_CONFIG_FIELD_INT32  = 1,  /* signed 32-bit decimal */
    HAL_CONFIG_FIELD_UINT32 = 2,  /* unsigned 32-bit decimal */
    HAL_CONFIG_FIELD_FLOAT64= 3,  /* double precision */
    HAL_CONFIG_FIELD_STRING = 4,  /* arbitrary string */
} HalConfigFieldType;

/* --------------------------------------------------------------------
 * Privacy mask
 * -------------------------------------------------------------------- */

#define HAL_PM_MAX_LABELS 8     /* max masked labels per dynamic config */
#define HAL_PM_LABEL_LEN  64    /* max label string length (incl. NUL) */

/** A single privacy mask region defined by up to 8 polygon vertices. */
typedef struct {
    const char *id;             /* unique identifier for this mask region */
    const char *name;           /* human-readable label */
    bool        is_enabled;     /* whether this region is active */
    struct {
        float x;                /* normalized X coordinate [0.0 .. 1.0] */
        float y;                /* normalized Y coordinate [0.0 .. 1.0] */
    } points[8];                /* polygon vertices (up to 8); set x < 0.0f on the first unused slot */
} HalPrivacyMaskItem;

/** One AI detection (bbox) to attach to a frame for dynamic privacy masking.
 *  Coordinates are normalized [0..1] on the displayed (post-rotate/flip) frame;
 *  the HAL scales them to encoded-frame pixels. The blender masks a solid block
 *  over the bbox (detection path — no per-pixel mask needed). */
typedef struct {
    char  label[HAL_PM_LABEL_LEN]; /* detection label — must match a masked_labels entry to be drawn */
    float x;                       /* bbox x0 [0..1] */
    float y;                       /* bbox y0 [0..1] */
    float w;                       /* bbox width  [0..1] */
    float h;                       /* bbox height [0..1] */
    float score;                   /* detection confidence [0..1] */
} HalFrameDetection;

/** One AI semantic-segmentation mask to attach to a frame for dynamic privacy masking.
 *  Unlike HalFrameDetection, this carries a per-pixel bytemask so irregular shapes are
 *  masked exactly. The bytemask covers the bbox region; the blender scales it to the
 *  encoded frame. Coordinates are normalized [0..1] on the displayed frame.
 *
 *  class_id must match the label's entry in privacy_mask_config.label_to_class_id for the
 *  blender to draw the mask (segmentation path filter). */
typedef struct {
    char           label[HAL_PM_LABEL_LEN]; /* mask label — must match a masked_labels entry */
    uint16_t       class_id;                /* segmentor class id (must match label_to_class_id) */
    float          x;                       /* bbox x0 of the mask region [0..1] */
    float          y;                       /* bbox y0 [0..1] */
    float          w;                       /* bbox width  [0..1] */
    float          h;                       /* bbox height [0..1] */
    uint32_t       mask_w;                  /* bytemask width in pixels */
    uint32_t       mask_h;                  /* bytemask height in pixels */
    const uint8_t *mask;                    /* bytemask, mask_w*mask_h bytes; 0=transparent, nonzero=mask */
} HalFrameSegmentation;

/** Privacy mask configuration (fill color or pixelation blur). */
typedef struct {
    struct {
        uint8_t r;              /* red channel   [0..255] */
        uint8_t g;              /* green channel  [0..255] */
        uint8_t b;              /* blue channel   [0..255] */
    } color;                    /* solid fill color (used when blur_radius == 0) */
    int blur_radius;            /* pixelation blur radius [2..64]; 0 = solid fill with color */
    HalPrivacyMaskItem *items;  /* array of mask regions */
    uint32_t item_count;        /* number of items in the array */

    /* Dynamic privacy mask (AI-driven): when dynamic_enabled is true the media library
     * blender consumes per-frame AI metadata attached to the encoder input buffer and masks
     * the detected regions. masked_labels selects which labels are masked.
     * Note: style (color/blur) is still global per stream — the media library v1.12.0
     * static-mask API carries style on the encoded output stream, not per region. */
    bool    dynamic_enabled;    /* enable AI-driven dynamic masking */
    uint32_t dilation_size;     /* dilation applied around each dynamic ROI (px) */
    char    masked_labels[HAL_PM_MAX_LABELS][HAL_PM_LABEL_LEN]; /* labels to mask */
    uint32_t num_masked_labels; /* number of populated masked_labels entries */
    /* Per-label segmentor class id (parallel to masked_labels). Required by the segmentation
     * path: the blender drops a semantic mask unless its class_id matches the mapped id here.
     * Detection-only path ignores this. Set to 0 for single-class segmentors. */
    int32_t masked_label_class_ids[HAL_PM_MAX_LABELS];
} HalPrivacyMaskConfig;

/* --------------------------------------------------------------------
 * Media image configuration
 * -------------------------------------------------------------------- */

/**
 * Image-level parameters that can be changed at runtime via
 * dynamic_change_image_config().
 *
 * Constraints:
 *   - digital_zoom and privacy_mask are mutually exclusive.
 */
typedef struct {
    HalRotationAngle rotation_angle;    /* image rotation */
    HalFlipDirection flip_direction;    /* image flip / mirror */

    bool digital_zoom;                  /* enable digital zoom (mutually exclusive with privacy_mask) */
    int  digital_zoom_value;            /* zoom magnification level [1..31] (only used when digital_zoom == true) */

    bool dewarp;                        /* enable Lens Distortion Correction (LDC) */
    bool dis;                           /* enable Digital Image Stabilization */
    bool eis;                           /* enable Electronic Image Stabilization (requires gyro) */
    bool grayscale;                     /* force grayscale output (discard chroma) */

    bool privacy_mask;                  /* enable privacy mask overlay (mutually exclusive with digital_zoom) */
    HalPrivacyMaskConfig privacy_mask_config;   /* privacy mask details (only used when privacy_mask == true) */
} HalMediaImageConfig;

/* --------------------------------------------------------------------
 * Per-stream parameter override
 * -------------------------------------------------------------------- */

/**
 * Override parameters for a single stream (frontend input + encoder output).
 *
 * Fields set to 0 (or NULL for codec) mean "no change".
 * All overrides are applied atomically via set_override_parameters().
 */
typedef struct {
    char stream_id[64];         /* pipeline stream id, e.g. "sink0" */

    /* Frontend input stream overrides (0 = no change) */
    uint32_t input_width;       /* frontend output width */
    uint32_t input_height;      /* frontend output height */
    uint32_t input_framerate;   /* frontend output framerate */

    /* Encoder overrides (0 / NULL = no change) */
    uint32_t encoder_width;     /* encoder input width  */
    uint32_t encoder_height;    /* encoder input height */
    uint32_t encoder_framerate; /* encoder input framerate */
    uint32_t encoder_bitrate;   /* target bitrate in bps */
    uint32_t encoder_gop;       /* GOP size */
    char     encoder_codec[16]; /* "h264" or "h265", empty string = no change */
} HalStreamOverride;

/**
 * Batch override for multiple streams.
 *
 * Passed to override_stream_params().
 */
typedef struct {
    HalStreamOverride *streams;
    uint32_t           stream_count;
} HalStreamOverrideBatch;

/* --------------------------------------------------------------------
 * Media configuration
 * -------------------------------------------------------------------- */

/**
 * Configuration passed to media init().
 *
 * Priority: config_json > config_path > platform default.
 * If image_config fields are non-zero they override corresponding values
 * parsed from the JSON / file.
 *
 * On Hailo-15, when both @ref config_path and @ref config_json are NULL/empty, init() falls
 * back to a compiled-in default media-library config (the SDK webserver config for the Basic
 * profiles), baked into the HAL .so and extracted to a scratch dir at runtime — no on-device
 * config path required. Other platforms require at least one to be set.
 *
 * @ref backup_folder_path (Hailo-15 / MediaLibrary):
 * - If non-NULL and non-empty: HAL sets the MediaLibrary default backup directory to this path,
 *   then calls @c MediaLibrary::initialize(config, @c true) so a prior JSON backup under that folder
 *   may be loaded when @c medialib_config.json exists there (HML behavior).
 * - If NULL or empty: backup directory comes only from @c medialib_config.json when present, and
 *   @c initialize(..., @c false) is used (no restore-from-backup at startup).
 */
typedef struct {
    const char *backup_folder_path;     /**< optional; see struct description above */
    const char *config_path;            /* filesystem path to the media library JSON config */
    const char *config_json;            /* in-memory JSON string (takes priority over config_path) */
    const char *encoder_overrides_json; /* per-stream encoder dimension overrides (JSON array) */

    HalMediaImageConfig image_config;   /* image overrides applied after JSON parsing */

    void *priv;                         /* platform-specific extension (opaque) */
} HalMediaConfig;

/**
 * Runtime request to add one frontend output stream to the active media
 * profile. The stream id is supplied out-of-band because HalVideoConfig does
 * not carry a MediaLibrary stream identifier.
 */
typedef struct {
    const char *stream_id;              /* new unique frontend stream id */
    HalVideoConfig video;               /* width/height/fps/format/pool; path/media_ptr ignored */
} HalMediaAddVideoConfig;

/**
 * Runtime request to add one encoder stream to the active media profile. The
 * implementation derives any platform-private encoder fields from an existing
 * encoder of the same family, then applies the public HalCodecConfig values.
 *
 * Added codec streams are independent by default: implementations should
 * disable automatic frontend->encoder forwarding for the new stream id unless
 * the caller explicitly enables it later.
 */
typedef struct {
    const char *stream_id;              /* new unique encoder stream id */
    HalCodecConfig codec;               /* packet type/size/rc fields; path/media_ptr ignored */
} HalMediaAddCodecConfig;

typedef struct {
    const char *stream_id;              /* existing frontend stream id to remove */
} HalMediaRemoveVideoConfig;

typedef struct {
    const char *stream_id;              /* existing encoder stream id to remove */
} HalMediaRemoveCodecConfig;

/* --------------------------------------------------------------------
 * Pipeline reconfiguration (stream count change)
 * -------------------------------------------------------------------- */

/**
 * Description of a single stream for pipeline reconfiguration.
 * Used when rebuilding the pipeline with a different number of streams.
 */
typedef struct {
    /* Frontend input (sensor output / DSP scaler) */
    uint32_t input_width;
    uint32_t input_height;
    uint32_t input_framerate;

    /* Encoder output */
    char     stream_id[64];         /* pipeline stream id, e.g. "sink0" */
    char     codec[16];             /* "h264" or "h265" */
    uint32_t encoder_width;
    uint32_t encoder_height;
    uint32_t encoder_framerate;
    uint32_t encoder_bitrate;       /* target bitrate in bps */
    uint32_t encoder_gop;           /* GOP size */
} HalPipelineStreamConfig;

/**
 * Full pipeline reconfiguration request.
 *
 * Triggers pipeline deinit → reinit with new config → start.
 * This is the only way to change the number of streams at runtime.
 * Expect ~2s interruption with no video output during reconfiguration.
 */
typedef struct {
    HalPipelineStreamConfig *streams;
    uint32_t                 stream_count;    /* number of streams (1-4) */
} HalPipelineReconfig;

/* --------------------------------------------------------------------
 * Motion detection
 * -------------------------------------------------------------------- */

/** Motion detection sensitivity. */
typedef enum {
    HAL_MOTION_SENSITIVITY_LOWEST = 0,
    HAL_MOTION_SENSITIVITY_LOW,
    HAL_MOTION_SENSITIVITY_MEDIUM,
    HAL_MOTION_SENSITIVITY_HIGH,
    HAL_MOTION_SENSITIVITY_HIGHEST,
} HalMotionSensitivity;

/**
 * Motion detection configuration (frontend frame-difference engine).
 *
 * Hailo mapping: medialib application_settings.motion_detection
 * (motion_detection_config_t). Enabling allocates an internal low-resolution
 * analysis stream, so toggling @c enabled may briefly restart the pipeline.
 *
 * An all-zero roi means full frame.
 *
 * @note ROI coordinates are pixels on the motion analysis stream (the
 * smallest output stream), not the main/full-resolution sensor image.
 */
typedef struct {
    bool     enabled;
    int32_t  roi_x, roi_y, roi_w, roi_h;  /* analysis-stream pixels; all-zero = full frame */
    HalMotionSensitivity sensitivity;     /* LOWEST..HIGHEST */
    float    threshold;                   /* 0..1 changed-pixel ratio in ROI to trigger */
} HalMotionConfig;

/**
 * Motion state change callback.
 *
 * Fires on state transitions (no-motion -> motion, motion -> no-motion) of
 * the frontend analysis, not on every frame.
 *
 * @param media_ctx       Media context the event belongs to.
 * @param motion_detected true when motion started, false when it stopped.
 * @param frame_id        Monotonic frontend frame counter.
 * @param timestamp_ns    Frame timestamp (nanoseconds).
 * @param userdata        Opaque pointer registered at subscribe time.
 *
 * @note Invoked from an internal worker thread; return quickly.
 */
typedef void (*HalMotionCallback)(void *media_ctx, bool motion_detected,
                                  uint64_t frame_id, uint64_t timestamp_ns, void *userdata);

/* --------------------------------------------------------------------
 * Thermal throttling
 * -------------------------------------------------------------------- */

/**
 * Pipeline throttling state.
 * Maps to Hailo Media Library @c media_library_throttling_state_t.
 *
 * The SoC thermal manager degrades performance stepwise (S0..S4) when the
 * chip heats up and recovers through COOLING back to FULL_PERFORMANCE.
 */
typedef enum {
    HAL_THROTTLING_UNINITIALIZED = 0,
    HAL_THROTTLING_FULL_PERFORMANCE,     /* no restriction */
    HAL_THROTTLING_COOLING,              /* recovering towards full performance */
    HAL_THROTTLING_S0,                   /* restriction level 0 (mildest) */
    HAL_THROTTLING_S1,
    HAL_THROTTLING_S2,
    HAL_THROTTLING_S3,
    HAL_THROTTLING_S4,                   /* restriction level 4 (strongest) */
} HalThrottlingState;

/**
 * Thermal throttling state change callback.
 *
 * @param media_ctx    Media context the event belongs to.
 * @param state        New throttling state.
 * @param profile_name Active profile after the restriction was applied
 *                     (platform string; may be empty), NUL-terminated.
 * @param userdata     Opaque pointer registered at subscribe time.
 *
 * @note Invoked from an internal worker thread; return quickly.
 */
typedef void (*HalThrottlingCallback)(void *media_ctx, HalThrottlingState state,
                                      const char *profile_name, void *userdata);

/* --------------------------------------------------------------------
 * Media operations table
 * -------------------------------------------------------------------- */

/**
 * Function-pointer table for media pipeline operations.
 * Platform implementations populate HAL_MEDIA_OPS at link time.
 */
typedef struct {
    /**
     * @brief Initialize the media pipeline.
     * @param config   Pipeline configuration (copied internally). See @ref HalMediaConfig.backup_folder_path
     *                 for optional Hailo MediaLibrary backup / restore-at-init behavior.
     * @param media_ctx_return  Receives the allocated media context on success.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*init)(const HalMediaConfig *config, void **media_ctx_return);

    /**
     * @brief Tear down the media pipeline and free all resources.
     * @param media_ctx  Context returned by init().
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*deinit)(void *media_ctx);

    /**
     * @brief Start the media pipeline (begins frame capture and encoding).
     * @param media_ctx  Initialized media context.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*start)(void *media_ctx);

    /**
     * @brief Stop the media pipeline.
     * @param media_ctx  Running media context.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*stop)(void *media_ctx);

    /**
     * @brief Query the current pipeline status.
     * @param media_ctx  Media context.
     * @return HalStatus value (cast from int).
     */
    int (*get_status)(void *media_ctx);

    /**
     * @brief Get the name of the currently active profile.
     * @param media_ctx    Media context.
     * @param profile_name Receives a pointer to the profile name string
     *                     (valid until the next profile switch or deinit).
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_current_profile)(void *media_ctx, char **profile_name);

    /**
     * @brief List all available profile names.
     * @param media_ctx        Media context.
     * @param profile_list     Receives an array of profile name strings.
     * @param profile_list_count Receives the number of profiles.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_profile_list)(void *media_ctx, char **profile_list, uint32_t *profile_list_count);

    /**
     * @brief Switch to a named profile.
     *
     * Objects (frontend, encoders) are reconfigured in-place; existing
     * callbacks remain registered.  Video and codec contexts obtained via
     * get_video_list / get_codec_list will have their configs updated
     * automatically.
     *
     * @param media_ctx      Media context.
     * @param profile_name   Target profile name (must exist in profile list).
     * @param force_recycle  When true, force a full stop_pipeline → start_pipeline recycle even when the
     *                       previous and target profiles share the same stream layout (normally skipped).
     *                       Used by rollback after a post-switch frame-verify failure to reset a media
     *                       graph that reports RUNNING but emits 0 frames.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*switch_profile)(void *media_ctx, const char *profile_name, bool force_recycle);

    /**
     * @brief Get the list of video contexts managed by the media pipeline.
     *
     * Each returned context has type HAL_VIDEO_TYPE_FROM_MEDIA and must
     * NOT be initialized or deinitialized by the caller.
     *
     * @param media_ctx        Media context.
     * @param video_list       Receives an array of (HalVideoContext *) pointers.
     * @param video_list_count Receives the number of video contexts.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_video_list)(void *media_ctx, void **video_list, uint32_t *video_list_count);

    /**
     * @brief Get the list of codec contexts managed by the media pipeline.
     *
     * Each returned context has type HAL_CODEC_TYPE_FROM_MEDIA and must
     * NOT be initialized or deinitialized by the caller.
     *
     * @param media_ctx        Media context.
     * @param codec_list       Receives an array of (HalCodecContext *) pointers.
     * @param codec_list_count Receives the number of codec contexts.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_codec_list)(void *media_ctx, void **codec_list, uint32_t *codec_list_count);

    /**
     * @brief Get a copy of the current media configuration.
     * @param media_ctx Media context.
     * @param config    Receives the current configuration.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_current_config)(void *media_ctx, HalMediaConfig *config);

    /**
     * @brief Get the current media-library profile JSON (platform-specific).
     *
     * For Hailo-15 MediaLibrary this maps to MediaLibrary::get_current_profile_str()
     * and returns a JSON string describing the active profile including any
     * runtime overrides applied via dynamic_change_image_config() or
     * set_override_parameters().
     *
     * @param media_ctx Media context.
     * @param json_out  Receives pointer to a null-terminated JSON string
     *                  owned by the implementation; valid until the next
     *                  profile change or deinit().
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_current_profile_json)(void *media_ctx, const char **json_out);

    /**
     * @brief Persist current media-library profiles to a directory (JSON tree).
     *
     * On Hailo-15 this wraps MediaLibrary @c set_default_backup_folder_path plus @c backup_profiles()
     * (HML persistent backup). The directory is created if needed; then the library default backup path
     * is restored to the effective default from @c init() (HAL @ref HalMediaConfig.backup_folder_path if set,
     * otherwise @c backup_folder_path from medialib JSON).
     *
     * @param media_ctx Media context.
     * @param path      Backup directory; if NULL or empty, uses @ref HalMediaConfig.backup_folder_path from
     *                  @c init() when that was non-empty, else JSON @c backup_folder_path when parsed at init;
     *                  if still unset, returns @c HAL_ERR_INVALID_ARG.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*backup_current_profile)(void *media_ctx, const char *path);

    /**
     * @brief Patch a single configuration field of the active media-library profile at runtime.
     *
     * Fetches the current profile, modifies a single field located by @p field_path inside its
     * JSON form, and re-applies the profile (on Hailo-15 this goes through the same path as
     * dynamic_change_image_config / override_stream_params, i.e. set_override_parameters with
     * stream-restart / encoder re-sync handled internally), so the change takes effect
     * immediately and is persisted with the profile.
     *
     * Path resolution (@p field_path is a dotted path, segments separated by '.'):
     *  - The actual location of the field is discovered from the real config content. A leading
     *    conceptual namespace token may be used to scope the field and is stripped before lookup:
     *      "frontend.<rest>"  -> frontend-config scoped (e.g. "frontend.hailort.use-hailort-service"
     *                            is auto-located under "application_settings").
     *      "encoder.<rest>"   -> encoder-config scoped.
     *  - An explicit profile path also works, e.g.
     *      "application_settings.hailort.use-hailort-service".
     *
     * @param media_ctx   Media context.
     * @param field_path  Dotted field path (see resolution rules above). Must be non-NULL.
     * @param field_type  Expected value type; controls how @p field_value is interpreted.
     * @param field_value Value encoded as a string. Must be non-NULL.
     * @return 0 on success, negative HalErrorCode on failure
     *         (HAL_ERR_INVALID_ARG for an unresolvable path or a value that does not
     *         match @p field_type).
     */
    int (*set_config_field)(void *media_ctx, const char *field_path,
                            HalConfigFieldType field_type, const char *field_value);

    /**
     * @brief Read a single scalar configuration field of the active media-library profile.
     *
     * Uses the same dotted-path resolution rules as set_config_field(): a leading
     * "frontend." / "encoder." namespace is stripped and the actual parent section is
     * auto-discovered from the real config content. The value reflects the current
     * (possibly runtime-overridden) profile, i.e. it sees changes applied via
     * set_config_field() / dynamic_change_image_config() / etc.
     *
     * Only scalar fields (bool / number / string) can be read; object, array and null
     * leaves return HAL_ERR_INVALID_ARG.
     *
     * @param media_ctx   Media context.
     * @param field_path  Dotted field path (see set_config_field() resolution rules). Non-NULL.
     * @param type_out    If non-NULL, receives the detected value type.
     * @param value_out   Receives pointer to a null-terminated string owned by the
     *                    implementation; valid until the next get_config_field() /
     *                    profile change or deinit(). Must be non-NULL.
     * @return 0 on success, negative HalErrorCode on failure
     *         (HAL_ERR_INVALID_ARG for an unresolvable path or non-scalar field).
     */
    int (*get_config_field)(void *media_ctx, const char *field_path,
                            HalConfigFieldType *type_out, const char **value_out);

    /**
     * @brief Dynamically change image parameters at runtime.
     *
     * Only the fields that differ from current will be applied.
     * Some changes (e.g. rotation) may trigger an internal stream reset.
     *
     * @param media_ctx Media context.
     * @param config    New image configuration.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*dynamic_change_image_config)(void *media_ctx, const HalMediaImageConfig *config);

    /**
     * @brief Add one frontend output stream to the active media profile.
     *
     * This creates a new FROM_MEDIA video context visible via get_video_list().
     * On Hailo MediaLibrary, note that application_input_streams.format is a
     * profile-wide setting; changing video.format may affect all frontend
     * outputs, not only the newly added stream.
     *
     * @param media_ctx Media context.
     * @param config    New frontend stream request.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*add_video_stream)(void *media_ctx, const HalMediaAddVideoConfig *config);

    /**
     * @brief Add one encoder stream to the active media profile.
     *
     * This creates a new FROM_MEDIA codec context visible via get_codec_list().
     * Added codec streams are independent by default; implementations should
     * leave automatic frontend->encoder forwarding disabled for the new stream.
     *
     * @param media_ctx Media context.
     * @param config    New encoder stream request.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*add_codec_stream)(void *media_ctx, const HalMediaAddCodecConfig *config);

    /**
     * @brief Atomically add both codec and frontend streams in a single profile update.
     *
     * Equivalent to calling add_codec_stream + add_video_stream but commits both
     * changes in one apply_profile_override_and_refresh, avoiding a double
     * pipeline stop/start that can exhaust DMA buffers.
     *
     * @param media_ctx Media context.
     * @param codec_cfg Encoder stream config (required, must not be NULL).
     * @param video_cfg Frontend stream config (optional, may be NULL).
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*add_streams_batch)(void *media_ctx, const HalMediaAddCodecConfig *codec_cfg,
                             const HalMediaAddVideoConfig *video_cfg);

    /**
     * @brief Remove one frontend output stream from the active media profile.
     *
     * This destroys the corresponding HAL video context (FROM_MEDIA) and
     * updates internal frontend subscriptions.
     *
     * After removal, the stream id is no longer present in get_video_list().
     */
    int (*remove_video_stream)(void *media_ctx, const HalMediaRemoveVideoConfig *config);

    /**
     * @brief Remove one encoder stream from the active media profile.
     *
     * This destroys the corresponding HAL codec context (FROM_MEDIA) and
     * updates internal encoder subscriptions.
     *
     * After removal, the stream id is no longer present in get_codec_list().
     */
    int (*remove_codec_stream)(void *media_ctx, const HalMediaRemoveCodecConfig *config);

    /**
     * @brief Atomically remove both codec and frontend streams in a single profile update.
     *
     * Equivalent to calling remove_codec_stream + remove_video_stream but commits both
     * removals in one apply_profile_override_and_refresh, avoiding a double pipeline
     * stop/start.
     *
     * @param media_ctx Media context.
     * @param stream_id Stream id to remove (must not be NULL).
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*remove_streams_batch)(void *media_ctx, const char *stream_id);

    /**
     * @brief Enable or disable automatic forwarding of frontend frames to encoders with the same stream id.
     *
     * When enabled (default), each frontend output buffer is also passed to the matching encoder
     * (MediaLibraryEncoder::add_buffer). When disabled, the application must feed encoders explicitly,
     * e.g. from a video callback via HAL_CODEC_OPS.input_frame.
     *
     * @param media_ctx Media context.
     * @param enable    true to enable auto-feed; false to disable.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*set_encoder_auto_feed)(void *media_ctx, bool enable);

    /**
     * @brief Query whether automatic frontend-to-encoder forwarding is enabled.
     *
     * @param media_ctx  Media context.
     * @param enable_out Receives the current flag.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_encoder_auto_feed)(void *media_ctx, bool *enable_out);

    /**
     * @brief Enable or disable automatic frontend-to-encoder forwarding for a specific encoder stream id.
     *
     * This overrides the global default configured via set_encoder_auto_feed() for the given @p stream_id.
     * If not set for a stream, the global default applies.
     *
     * @param media_ctx  Media context.
     * @param stream_id  Encoder/frontend stream id to configure (must match the encoder id used by the pipeline).
     * @param enable     true to enable auto-feed for this stream; false to disable.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*set_encoder_auto_feed_for_stream)(void *media_ctx, const char *stream_id, bool enable);

    /**
     * @brief Query whether automatic frontend-to-encoder forwarding is enabled for a specific stream id.
     *
     * If a per-stream override was configured via set_encoder_auto_feed_for_stream(), returns that value.
     * Otherwise returns the global default configured via set_encoder_auto_feed().
     *
     * @param media_ctx   Media context.
     * @param stream_id   Encoder/frontend stream id to query.
     * @param enable_out  Receives the current flag for this stream.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_encoder_auto_feed_for_stream)(void *media_ctx, const char *stream_id, bool *enable_out);

    /**
     * @brief Override per-stream parameters at runtime without switching profiles.
     *
     * Applies resolution, framerate, bitrate, GOP, and codec overrides to the
     * specified streams.  The changes are applied atomically via
     * set_override_parameters() and take effect without a full pipeline restart
     * (brief encoder re-sync ~50-100ms may occur for codec/resolution changes).
     *
     * @param media_ctx  Media context.
     * @param batch      Array of per-stream overrides and count.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*override_stream_params)(void *media_ctx, const HalStreamOverrideBatch *batch);

    /**
     * @brief Reconfigure the pipeline with a new stream layout (count/resolution/codec).
     *
     * Tears down the running pipeline, generates a new config from the supplied
     * stream descriptions, re-initializes and restarts the pipeline.
     * Video and codec contexts are rebuilt; all subscriber callbacks remain valid.
     *
     * Expect ~2s interruption with no video output.
     *
     * @param media_ctx  Media context.
     * @param reconfig   New pipeline layout.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*reconfigure_pipeline)(void *media_ctx, const HalPipelineReconfig *reconfig);

    /**
     * @brief Get the media HAL version string.
     * @return Static version string e.g. "Hailo15 HAL-MEDIA 2.0.0".
     */
    const char *(*get_version)(void);

    /**
     * @brief Attach AI results (detections and/or semantic-segmentation masks) to a frame so the
     *        media library DSP privacy-mask blender (dynamic path) masks the regions on encode.
     *
     * Builds a single AnalyticsMetadata on the frame carrying both arrays (detections → solid-block
     * masks; segmentations → irregular per-pixel masks). Replaces any metadata previously attached
     * to the frame; pass 0 for both counts (or NULL arrays) to clear.
     *
     * The upper layer decides which frame to attach to and what to attach — typically called from a
     * HalVideoFrameCallback on the HalFrameBuffer it receives, before the frame reaches the encoder
     * (the frontend bridge auto-feeds via add_buffer immediately after the callback; for manual
     * encode, call this before hal_codec_encode_frame). The HAL copies any bytemask into its own
     * storage pinned for the duration of the DSP call, so caller memory may be freed after return.
     *
     * Requires privacy_mask_config.dynamic_enabled=true and matching masked_labels entries for the
     * blender to actually draw. Segmentation entries additionally require label_to_class_id config.
     *
     * @param media_ctx Media context.
     * @param frame     HalFrameBuffer from a video subscribe callback (carries the underlying ml buffer).
     * @param dets      Detection array (may be NULL when det_count==0).
     * @param det_count Number of detections.
     * @param segs      Segmentation array (may be NULL when seg_count==0).
     * @param seg_count Number of segmentation masks.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*attach_frame_analytics)(void *media_ctx, HalFrameBuffer *frame,
                                  const HalFrameDetection *dets, uint32_t det_count,
                                  const HalFrameSegmentation *segs, uint32_t seg_count);

    /* ---------- thermal throttling events (M1 additions) ---------- */

    /**
     * @brief Subscribe to thermal throttling state changes.
     *
     * The callback fires when the SoC thermal manager restricts or restores
     * pipeline performance (e.g. AI-ISP gated off under heat — previously
     * only visible as HAL_ERR_PROFILE_RESTRICTED on switch_profile()).
     *
     * Only one subscriber is supported; a second subscribe replaces the first.
     *
     * @param media_ctx Media context.
     * @param callback  State change callback.
     * @param userdata  Opaque pointer passed to the callback.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*subscribe_throttling)(void *media_ctx, HalThrottlingCallback callback, void *userdata);

    /**
     * @brief Unsubscribe from throttling state changes.
     * @param media_ctx Media context.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*unsubscribe_throttling)(void *media_ctx);

    /**
     * @brief Query the current thermal throttling state (poll variant).
     * @param media_ctx Media context.
     * @param state_out Receives the current state.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_throttling_state)(void *media_ctx, HalThrottlingState *state_out);

    /* ---------- motion detection (M2 additions) ---------- */

    /**
     * @brief Configure the frontend motion detection engine.
     *
     * @param media_ctx Media context.
     * @param config    Motion configuration; enabled=false disables detection.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*set_motion_config)(void *media_ctx, const HalMotionConfig *config);

    /**
     * @brief Retrieve the current motion detection configuration.
     * @param media_ctx Media context.
     * @param config    Receives the current configuration.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_motion_config)(void *media_ctx, HalMotionConfig *config);

    /**
     * @brief Subscribe to motion state change events (transition-triggered).
     *
     * @param media_ctx Media context.
     * @param callback  Motion callback.
     * @param userdata  Opaque pointer passed to the callback.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*subscribe_motion)(void *media_ctx, HalMotionCallback callback, void *userdata);

    /**
     * @brief Unsubscribe from motion events.
     * @param media_ctx Media context.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*unsubscribe_motion)(void *media_ctx);
} HalMediaOps;

/** Platform-specific media operations (resolved at link time). */
extern HalMediaOps HAL_MEDIA_OPS;

#ifdef __cplusplus
}
#endif
