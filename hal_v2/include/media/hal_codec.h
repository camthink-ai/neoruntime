/**
 * @file hal_codec.h
 * @brief HAL Codec - Video encoder hardware abstraction layer.
 *
 * Supports multiple encoder backends:
 *   - HAL_CODEC_TYPE_HW:         platform hardware encoder
 *   - HAL_CODEC_TYPE_SOFT:       software encoder (e.g. MJPEG)
 *   - HAL_CODEC_TYPE_FROM_MEDIA: obtained from media pipeline (get_codec_list)
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
 * Codec types
 * -------------------------------------------------------------------- */

/** Encoder backend type. */
typedef enum {
    HAL_CODEC_TYPE_HW = 0,         /* platform hardware encoder */
    HAL_CODEC_TYPE_SOFT,           /* software encoder */
    HAL_CODEC_TYPE_FROM_MEDIA,     /* obtained from media library (see HalMediaOps.get_codec_list) */
    HAL_CODEC_TYPE_MAX,            /* sentinel (not a valid type) */
} HalCodecType;

/* --------------------------------------------------------------------
 * Rate control
 * -------------------------------------------------------------------- */

/** Rate control mode for the encoder. */
typedef enum {
    HAL_RC_CBR = 0,                /* constant bitrate */
    HAL_RC_VBR,                    /* variable bitrate */
    HAL_RC_CVBR,                   /* constrained variable bitrate */
    HAL_RC_CQP,                    /* constant quantization parameter */
} HalRateControlMode;

/* --------------------------------------------------------------------
 * Codec configuration
 * -------------------------------------------------------------------- */

/**
 * Encoder configuration.
 *
 * For HW / SOFT types: all fields are set by the caller before init().
 * For FROM_MEDIA type: fields are populated by media->get_codec_list()
 * and reflect the encoder stream configuration from the media pipeline.
 */
typedef struct {
    HalCodecType        type;           /* encoder backend type */
    HalPacketType       packet_type;    /* output encoded format (H264 / H265 / MJPEG) */
    char               *path;           /* device path (NULL for FROM_MEDIA) */
    void               *media_ptr;      /* pointer to parent HalMediaContext (only for FROM_MEDIA) */

    /* input video parameters */
    uint32_t            width;          /* input frame width in pixels */
    uint32_t            height;         /* input frame height in pixels */
    HalPixelFormat      format;         /* input pixel format (typically NV12) */
    uint32_t            framerate;      /* input frame rate (fps) */

    /* rate control */
    HalRateControlMode  rc_mode;        /* rate control strategy */
    uint32_t            bitrate;        /* target bitrate in bps (for CBR / VBR / CVBR) */
    uint32_t            qp;             /* fixed QP value (for CQP mode) */
    uint32_t            qp_min;         /* minimum QP [1..51], 0 = platform default (typically 10) */
    uint32_t            qp_max;         /* maximum QP [1..51], 0 = platform default (typically 48) */
    int                 qp_hdr;         /* QP for HDR content [1..51], 0 = platform default (-1) */
    int                 intra_qp_delta; /* intra frame QP offset [-51..51], 0 = no offset */
    int                 fixed_intra_qp; /* force intra QP to this value [0..51], 0 = auto */

    /* GOP / rate-control IDR spacing (Hailo maps to gop_config vs rate_control separately). */
    uint32_t            gop_size;                 /* gop_config.gop_size; 0 = leave unchanged / derive from JSON */
    uint32_t            rate_control_gop_length; /* rate_control.gop_length; 0 = use gop_size when that is non-zero */
    uint32_t            intra_pic_rate;          /* rate_control.intra_pic_rate; 0 = use gop_size when that is non-zero */
    uint32_t            b_frames;                /* number of B-frames per GOP, 0 = none */

    /* JPEG-specific */
    uint32_t            jpeg_quality;   /* JPEG quality [1..100], only for HAL_PACKET_TYPE_MJPEG */

    void               *priv;           /* platform-specific extension (opaque) */
} HalCodecConfig;

/* --------------------------------------------------------------------
 * Smart encoding (ROI)
 * -------------------------------------------------------------------- */

/** Maximum number of coding ROIs supported by the HAL API. */
#define HAL_CODEC_ROI_MAX 10

/**
 * One coding region of interest (normalized coordinates).
 * The union of all ROIs is encoded at higher quality; the rest of the frame
 * gets @c background_qp_delta added to its QP (coarser, lower bitrate).
 */
typedef struct {
    float x;         /* left edge,   normalized [0..1] */
    float y;         /* top edge,    normalized [0..1] */
    float w;         /* width,       normalized [0..1] */
    float h;         /* height,      normalized [0..1] */
} HalCodecRoi;

/**
 * ROI / smart-encoder configuration.
 *
 * Maps to Hailo Media Library @c smart_encoder_config_t (SmartStream+).
 * Platform constraint (Hailo-15): H.264 + CVBR rate control only; other
 * codec / RC combinations return HAL_ERR_NOT_SUPPORTED.
 *
 * @note ROI config is an independent structure: HalCodecConfig is untouched.
 */
typedef struct {
    bool     enabled;              /* smart encoder master switch */
    int      background_qp_delta;  /* QP added outside ROIs [0..15]; 0 = platform default */
    uint32_t roi_count;            /* 0..HAL_CODEC_ROI_MAX when enabled; 0 = no static ROIs */
    HalCodecRoi rois[HAL_CODEC_ROI_MAX];
} HalCodecRoiConfig;

/* --------------------------------------------------------------------
 * Encoder stream statistics
 * -------------------------------------------------------------------- */

/**
 * Runtime statistics of one encoder stream.
 * Convention: fps = -1.0f and bitrate_kbps = 0 mean "unknown".
 *
 * fps comes from the encoder's measured output rate; bitrate_kbps is the
 * moving average over the monitor window (Hailo computes bytes/s internally,
 * converted to kilobits/s here). MJPEG streams have no bitrate monitor.
 */
typedef struct {
    float    fps;               /* measured encoder output fps, -1.0f if unknown */
    uint32_t bitrate_kbps;      /* moving-average bitrate in kbps, 0 if unknown */
    uint32_t monitor_period_s;  /* bitrate monitor averaging window in seconds, 0 if unknown */
} HalCodecStreamStats;

/* --------------------------------------------------------------------
 * Callback type
 * -------------------------------------------------------------------- */

/**
 * Encoded packet delivery callback for push (subscribe) mode.
 *
 * @param codec_ctx The codec context that produced the packet.
 * @param packet    Encoded packet; caller must call release_packet() when done.
 * @param userdata  Opaque pointer registered at subscribe time.
 *
 * @note The callback is invoked from an internal worker thread.
 *       Implementations should return quickly.
 */
typedef void (*HalCodecFrameCallback)(void *codec_ctx, HalPacketBuffer *packet, void *userdata);

/* --------------------------------------------------------------------
 * Codec operations table
 * -------------------------------------------------------------------- */

/**
 * Function-pointer table for encoder operations.
 * Platform implementations populate HAL_CODEC_OPS at link time.
 */
typedef struct {
    /**
     * @brief Initialize an encoder and allocate context.
     *
     * Not supported for HAL_CODEC_TYPE_FROM_MEDIA (returns HAL_ERR_NOT_SUPPORTED).
     *
     * @param config           Encoder configuration (copied internally).
     * @param codec_ctx_return Receives the allocated codec context pointer.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*init)(const HalCodecConfig *config, void **codec_ctx_return);

    /**
     * @brief Release an encoder and free its context.
     *
     * Not supported for HAL_CODEC_TYPE_FROM_MEDIA (returns HAL_ERR_NOT_SUPPORTED).
     *
     * @param codec_ctx Codec context to destroy.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*deinit)(void *codec_ctx);

    /**
     * @brief Start the encoder.
     *
     * For FROM_MEDIA type: start is delegated to the parent media pipeline.
     *
     * @param codec_ctx Initialized codec context.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*start)(void *codec_ctx);

    /**
     * @brief Stop the encoder.
     *
     * For FROM_MEDIA type: stop is delegated to the parent media pipeline.
     *
     * @param codec_ctx Running codec context.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*stop)(void *codec_ctx);

    /**
     * @brief Feed a raw video frame to the encoder.
     *
     * For FROM_MEDIA type on Hailo: this maps to encoder->add_buffer().
     * For HW / SOFT types: this submits the frame for encoding.
     *
     * @param codec_ctx Codec context.
     * @param frame     Raw video frame buffer.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*input_frame)(void *codec_ctx, HalFrameBuffer *frame);

    /**
     * @brief Subscribe to encoded packet delivery (push mode).
     * @param codec_ctx Codec context.
     * @param callback  Packet delivery callback.
     * @param userdata  Opaque pointer passed to the callback.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*subscribe)(void *codec_ctx, HalCodecFrameCallback callback, void *userdata);

    /**
     * @brief Unsubscribe from encoded packet delivery.
     * @param codec_ctx Codec context.
     * @param callback  The callback to remove (must match subscribe).
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*unsubscribe)(void *codec_ctx, HalCodecFrameCallback callback);

    /**
     * @brief Release an encoded packet buffer.
     *
     * Must be called for every packet obtained via subscribe callback.
     *
     * @param codec_ctx Codec context.
     * @param packet    Packet buffer to release.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*release_packet)(void *codec_ctx, HalPacketBuffer *packet);

    /**
     * @brief Query the current encoder status.
     * @param codec_ctx Codec context.
     * @return HalStatus value (cast from int).
     */
    int (*get_status)(void *codec_ctx);

    /**
     * @brief Get a snapshot of the current encoder configuration.
     * @param codec_ctx Codec context.
     * @param config    Receives the current configuration.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_current_config)(void *codec_ctx, HalCodecConfig *config);

    /**
     * @brief Dynamically change encoder parameters at runtime.
     *
     * For FROM_MEDIA type: mapped to media library set_override_parameters().
     * Only certain fields can be changed at runtime (e.g. bitrate, QP, GOP).
     *
     * @param codec_ctx Codec context.
     * @param config    New encoder configuration.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*dynamic_change_config)(void *codec_ctx, const HalCodecConfig *config);

    /**
     * @brief Get the codec HAL version string.
     * @return Static version string e.g. "Hailo15 HAL-CODEC 2.0.0".
     */
    const char *(*get_version)(void);

    /* ---------- FROM_MEDIA encoder registration ---------- */

    /**
     * @brief Initialize encoder from a pre-existing FROM_MEDIA codec context
     * @param codec_ctx Pre-created codec context from media pipeline (get_codec_list())
     * @param stream_name Stream identifier (e.g. "sink0")
     * @return >= 0: encoder handle (usable with subscribe/unsubscribe/etc), < 0: HalErrorCode
     *
     * Creates a HAL-internal context for the pre-existing encoder, subscribes to
     * the shared MediaLibrary's encoder output via bridge_cb, and registers in
     * the internal encoder map. Does NOT create a new encoder — the media pipeline
     * owns it. NULL = not supported by this HAL.
     */
    int (*init_from_context)(void *codec_ctx, const char *stream_name);

    /**
     * @brief Deinitialize a FROM_MEDIA encoder context
     * @param encoder_handle Handle from init_from_context
     * @return HalErrorCode
     *
     * Unsubscribes from MediaLibrary encoder output, removes from internal map,
     * but does NOT destroy the underlying encoder (owned by media pipeline).
     * NULL = not supported by this HAL.
     */
    int (*deinit_from_context)(int encoder_handle);

    /* ---------- smart encoding / runtime control (M1 additions) ---------- */

    /**
     * @brief Configure ROI / smart encoding for the encoder stream.
     *
     * Applies at runtime; takes effect from the next GOP (per Hailo
     * SmartStream+ behavior). Hailo-15 constraint: H.264 + CVBR only
     * (HAL_ERR_NOT_SUPPORTED otherwise). MJPEG streams are not supported.
     *
     * @param codec_ctx Codec context (HW or FROM_MEDIA).
     * @param config    ROI configuration; enabled=false disables smart encoding.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*set_roi_config)(void *codec_ctx, const HalCodecRoiConfig *config);

    /**
     * @brief Retrieve the current ROI / smart encoding configuration.
     * @param codec_ctx Codec context.
     * @param config    Receives the current configuration.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_roi_config)(void *codec_ctx, HalCodecRoiConfig *config);

    /**
     * @brief Force the next frame to be encoded as an intra (IDR) frame.
     *
     * Useful for snapshot-on-demand, stream joining, and error recovery.
     *
     * @param codec_ctx Codec context (HW or FROM_MEDIA).
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*force_idr)(void *codec_ctx);

    /**
     * @brief Query runtime statistics of the encoder stream.
     *
     * Returns current fps, moving-average bitrate and encoded frame count.
     * Fields the platform cannot determine are set to their "unknown" value
     * (see HalCodecStreamStats) instead of failing the whole call.
     *
     * @param codec_ctx Codec context (HW or FROM_MEDIA).
     * @param stats     Receives the statistics.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_stream_stats)(void *codec_ctx, HalCodecStreamStats *stats);
} HalCodecOps;

/** Platform-specific codec operations (resolved at link time). */
extern HalCodecOps HAL_CODEC_OPS;

#ifdef __cplusplus
}
#endif
