/**
 * @file hal_isp.h
 * @brief HAL ISP - Image Signal Processor control interface.
 *
 * Provides runtime control over ISP parameters such as exposure, white
 * balance, noise reduction, and image tuning (brightness, contrast, etc.).
 *
 * ISP operations take a video context pointer as the first argument.
 * For FROM_MEDIA type video contexts, ISP parameters are applied via the
 * media library's V4L2 control interface.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../common/hal_common.h"
#include "../common/hal_buffer.h"
#include "hal_video.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------
 * ISP manual image tuning
 * -------------------------------------------------------------------- */

/**
 * Manual image quality parameters.
 * When manual_state is true, the ISP uses the values below directly.
 * When false, the ISP's auto-adjustment algorithms manage these parameters.
 */
typedef struct {
    bool manual_state;      /* true: use values below; false: auto mode */

    int brightness;         /* brightness level [0..100] */
    int contrast;           /* contrast level [0..100] */
    int saturation;         /* color saturation level [0..100] */
    int sharpness;          /* edge sharpness level [0..100] */
} HalIspManualConfig;

/* --------------------------------------------------------------------
 * ISP exposure control
 * -------------------------------------------------------------------- */

/**
 * Exposure configuration.
 * When auto_exposure is true, the ISP's 3A engine manages exposure;
 * only backlight compensation is user-configurable.
 * When auto_exposure is false, exposure_time_us and gain are used directly.
 */
typedef struct {
    bool auto_exposure;     /* true: auto exposure (AE); false: manual exposure */

    int backlight;          /* backlight compensation [0..100] (only when auto_exposure == true) */
    int exposure_time_us;   /* exposure time in microseconds (only when auto_exposure == false) */
    int gain;               /* sensor gain (only when auto_exposure == false) */
} HalIspExposureConfig;

/* --------------------------------------------------------------------
 * Power-line frequency (anti-flicker)
 * -------------------------------------------------------------------- */

/** Power-line frequency for anti-flicker compensation. */
typedef enum {
    HAL_ISP_PWR_FREQ_OFF  = 0,     /* disabled */
    HAL_ISP_PWR_FREQ_50HZ = 1,     /* 50 Hz (Europe, Asia) */
    HAL_ISP_PWR_FREQ_60HZ = 2,     /* 60 Hz (Americas, Japan 60Hz regions) */
} HalIspPowerFreq;

/* --------------------------------------------------------------------
 * Combined ISP image configuration
 * -------------------------------------------------------------------- */

/**
 * Full ISP image configuration combining all ISP-related parameters.
 */
typedef struct {
    HalIspPowerFreq     pwr_freq;           /* anti-flicker frequency */

    int                 noise_reduction;    /* noise reduction strength [0..100] */
    int                 wdr_value;          /* Wide Dynamic Range strength [0..100] */

    int                 awb_idx;            /* index of the active AWB profile in awb_profile_list */
    /** Optional illuminant names (FROM_MEDIA: from ML aw_drv4 list); filled by get_current_image_config. */
    const char        **awb_profile_list;
    uint32_t            awb_profile_count;

    HalIspManualConfig  manual_config;      /* manual image tuning parameters */
    HalIspExposureConfig exposure_config;   /* exposure control parameters */
} HalIspImageConfig;

/* --------------------------------------------------------------------
 * ISP operations table
 * -------------------------------------------------------------------- */

/* --------------------------------------------------------------------
 * ISP AF (autofocus) statistics support
 * -------------------------------------------------------------------- */

/** Maximum number of AF measurement windows supported by the HAL API. */
#define HAL_ISP_AF_MAX_WINDOWS 3

/**
 * One AF measurement window.
 *
 * Aligns with the Hailo Imaging User Guide "6.3 AF":
 * - V4L2 control name: @c isp_af_window
 * - Format: int32_t 3x4 array, each window is {x,y,w,h} in pixels.
 */
typedef struct {
    int32_t x;  /* left   in pixels */
    int32_t y;  /* top    in pixels */
    int32_t w;  /* width  in pixels */
    int32_t h;  /* height in pixels */
} HalIspAfWindow;

/**
 * AF window configuration.
 *
 * When enabled, the ISP collects AF measurement statistics for the configured windows.
 */
typedef struct {
    bool         enabled;
    uint32_t     window_count; /* 1..HAL_ISP_AF_MAX_WINDOWS when enabled; 0 when disabled */
    HalIspAfWindow windows[HAL_ISP_AF_MAX_WINDOWS];
} HalIspAfWindowsConfig;

/**
 * AF measurement statistics returned by the ISP.
 *
 * Aligns with the Hailo Imaging User Guide "6.3 AF":
 * - V4L2 control name: @c isp_af_measurement
 * - Format: uint32_t 1x6 array: sum1,sum2,sum3,luma1,luma2,luma3
 *
 * On Hailo-15 ISP (typical usage):
 * - @c sum[i]: hardware focus / clarity energy from edge strength and contrast in window @c i;
 *   it reaches a **peak** at best focus — AF algorithms should seek a **maximum** (not a minimum).
 * - @c luma[i]: mean brightness in that window; use with @c sum (e.g. ratio) and keep exposure stable,
 *   because large brightness swings can skew @c sum readings.
 * All-zero readings often mean AF stats are not running (AF not enabled, invalid/no windows, or
 * window configuration overflow per tuning guide) — not “infinitely sharp” or “infinitely blurred”.
 */
typedef struct {
    uint32_t window_count; /* number of valid entries in arrays below */
    uint64_t frame_id;     /* 0 when unknown */
    uint64_t timestamp_ns; /* 0 when unknown */
    uint32_t sum[HAL_ISP_AF_MAX_WINDOWS];   /* sum[0]=sum1 ... focus energy; higher → sharper at peak focus */
    uint32_t luma[HAL_ISP_AF_MAX_WINDOWS];  /* luma[0]=luma1 ... mean luma in window */
} HalIspAfMeasurement;

/* --------------------------------------------------------------------
 * ISP manual white balance (M2)
 * -------------------------------------------------------------------- */

/**
 * Manual white balance configuration.
 *
 * When manual_state is true the AWB pipeline is frozen (awbv2 algorithm +
 * isp_awb_enable + isp_awb_mode) and the per-channel gains below are applied
 * directly (1.0 = neutral, Q8.8 controls). When false, auto white balance
 * runs and the gain fields are ignored.
 *
 * Control ranges (Hailo imaging guide 6.2): r/gr gains ~[0.39..4.0],
 * gb/b gains ~[0.39..1.56]; out-of-range values return HAL_ERR_INVALID_ARG.
 *
 * @note The optional 3x3 color-correction matrix (ccm, row-major, written to
 * isp_wb_cc_matrix) is only applied when the platform exposes the control;
 * an all-zero matrix (default) means "leave CCM unchanged".
 */
typedef struct {
    bool  manual_state;      /* true: use gains below; false: auto (AWB) */
    float r_gain;            /* red gain, 1.0 = neutral [~0.39..4.0] */
    float gr_gain;           /* green-red gain, 1.0 = neutral [~0.39..4.0] */
    float gb_gain;           /* green-blue gain, 1.0 = neutral [~0.39..1.56] */
    float b_gain;            /* blue gain, 1.0 = neutral [~0.39..1.56] */
    float ccm[9];            /* optional 3x3 CCM, row-major; all-zero = unchanged */
} HalIspWbConfig;

/* --------------------------------------------------------------------
 * ISP temporal noise reduction (M2)
 * -------------------------------------------------------------------- */

/**
 * 3DNR (temporal noise reduction) configuration.
 *
 * Hailo mapping: platform-dependent — the HAL probes the known V4L2 control
 * names (temporal NR is configured through the ISP media-server tuning
 * stack) and returns HAL_ERR_NOT_SUPPORTED when none is present.
 */
typedef struct {
    bool enabled;
    int  strength;           /* [0..100] */
} HalIspNr3dConfig;

/* --------------------------------------------------------------------
 * ISP AE statistics (M2)
 * -------------------------------------------------------------------- */

/** AE luma statistics grid dimension (5x5 mean-luma zones). */
#define HAL_ISP_AE_LUMA_GRID 25

/**
 * Auto-exposure statistics snapshot (poll model — same usage pattern as
 * get_af_measurement()).
 *
 * Hailo mapping: V4L2 controls isp_ae_hist (256-bin luma histogram) and
 * isp_ae_luma (5x5 mean-luma grid). Fields carry per-source validity flags
 * so a platform exposing only one of the two still returns partial data.
 */
typedef struct {
    uint32_t hist[256];                    /* luma histogram bins */
    uint32_t luma[HAL_ISP_AE_LUMA_GRID];   /* 5x5 mean luma grid */
    bool     hist_valid;                   /* false when the histogram control is absent */
    bool     luma_valid;                   /* false when the luma-grid control is absent */
} HalIspAeStats;

/**
 * Function-pointer table for ISP operations.
 * Platform implementations populate HAL_ISP_OPS at link time.
 *
 * All functions take a video_ctx pointer (HalVideoContext*) as the first
 * argument, since the ISP is associated with the video capture device.
 */
typedef struct {
    /**
     * @brief Apply a full ISP image configuration.
     * @param video_ctx Video context owning the ISP.
     * @param config    Full ISP configuration to apply.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*set_image_config)(void *video_ctx, const HalIspImageConfig *config);

    /**
     * @brief Retrieve the current full ISP image configuration.
     * @param video_ctx Video context owning the ISP.
     * @param config    Receives the current ISP configuration.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_current_image_config)(void *video_ctx, HalIspImageConfig *config);

    /**
     * @brief Apply manual image tuning parameters only.
     * @param video_ctx Video context owning the ISP.
     * @param config    Manual tuning configuration to apply.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*set_manual_config)(void *video_ctx, const HalIspManualConfig *config);

    /**
     * @brief Retrieve the current manual image tuning parameters.
     * @param video_ctx Video context owning the ISP.
     * @param config    Receives the current manual configuration.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_current_manual_config)(void *video_ctx, HalIspManualConfig *config);

    /**
     * @brief Apply exposure parameters only.
     * @param video_ctx Video context owning the ISP.
     * @param config    Exposure configuration to apply.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*set_exposure_config)(void *video_ctx, const HalIspExposureConfig *config);

    /**
     * @brief Retrieve the current exposure parameters.
     * @param video_ctx Video context owning the ISP.
     * @param config    Receives the current exposure configuration.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_current_exposure_config)(void *video_ctx, HalIspExposureConfig *config);

    /**
     * @brief Configure AF measurement windows.
     *
     * This enables/disables AF statistics generation and sets up to
     * @ref HAL_ISP_AF_MAX_WINDOWS windows.
     *
     * @param video_ctx Video context owning the ISP.
     * @param config    AF window configuration to apply.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*set_af_windows_config)(void *video_ctx, const HalIspAfWindowsConfig *config);

    /**
     * @brief Retrieve the current AF measurement window configuration.
     *
     * When supported, this reads the underlying V4L2 control (typically @c isp_af_window)
     * and fills @p config with the current window rectangles.
     *
     * @param video_ctx Video context owning the ISP.
     * @param config    Receives the current AF window configuration.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_af_windows_config)(void *video_ctx, HalIspAfWindowsConfig *config);

    /**
     * @brief Subscribe/unsubscribe to AF measurement availability notifications (event-driven mode).
     *
     * On platforms that support V4L2 events, enabling this allows the application to block/wait
     * until new AF statistics are ready, instead of polling @ref get_af_measurement.
     *
     * Platforms that do not implement event notification return HAL_ERR_NOT_SUPPORTED.
     *
     * @param video_ctx Video context owning the ISP.
     * @param enable    true to subscribe; false to unsubscribe.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*subscribe_af_measurement)(void *video_ctx, bool enable);

    /**
     * @brief Wait until AF measurement data is ready (event-driven mode).
     *
     * Requires a prior successful subscribe via @ref subscribe_af_measurement.
     *
     * @param video_ctx   Video context owning the ISP.
     * @param timeout_ms  Timeout in milliseconds. -1 = wait forever, 0 = non-blocking check.
     * @return HAL_OK when an AF measurement is ready; HAL_ERR_TIMEOUT on timeout;
     *         HAL_ERR_NOT_SUPPORTED when events are not supported; or other negative HalErrorCode.
     */
    int (*wait_af_measurement)(void *video_ctx, int timeout_ms);

    /**
     * @brief Read the latest AF measurement statistics (polling mode).
     *
     * Implementations may either return the most recent sample available,
     * or trigger a fresh read from the ISP depending on platform behavior.
     *
     * @param video_ctx Video context owning the ISP.
     * @param meas      Receives the latest AF measurement values.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_af_measurement)(void *video_ctx, HalIspAfMeasurement *meas);

    /**
     * @brief Get the ISP HAL version string.
     * @return Static version string e.g. "Hailo15 HAL-ISP 2.0.0".
     */
    const char *(*get_version)(void);

    /* ---------- M2 additions (appended; NULL = not supported) ---------- */

    /**
     * @brief Apply manual white balance gains (or return to auto AWB).
     *
     * @param video_ctx Video context owning the ISP.
     * @param config    WB configuration; manual_state=false restores auto AWB.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*set_wb_config)(void *video_ctx, const HalIspWbConfig *config);

    /**
     * @brief Retrieve the current white balance configuration.
     *
     * Reads back the effective per-channel gains and manual/auto state.
     *
     * @param video_ctx Video context owning the ISP.
     * @param config    Receives the current configuration.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_current_wb_config)(void *video_ctx, HalIspWbConfig *config);

    /**
     * @brief Configure temporal noise reduction (3DNR) independently of the
     *        spatial (2DNR) noise_reduction field in the image config.
     * @param video_ctx Video context owning the ISP.
     * @param config    3DNR configuration.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*set_3dnr_config)(void *video_ctx, const HalIspNr3dConfig *config);

    /**
     * @brief Read a snapshot of auto-exposure statistics (poll model).
     *
     * Mirrors get_af_measurement() usage: call periodically. Per-field
     * validity flags report which statistics the platform provides.
     *
     * @param video_ctx Video context owning the ISP.
     * @param stats     Receives the statistics snapshot.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_ae_stats)(void *video_ctx, HalIspAeStats *stats);

    /**
     * @brief Adjust HDR long/short exposure ratios at runtime.
     *
     * Only meaningful while an HDR pipeline is active; returns
     * HAL_ERR_INVALID_STATE otherwise.
     *
     * @param video_ctx Video context owning the ISP.
     * @param ls_ratio  long-to-short exposure ratio (e.g. 16.0).
     * @param vs_ratio  very-short exposure ratio (2DOL: ignored, pass 0).
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*set_hdr_ratios)(void *video_ctx, float ls_ratio, float vs_ratio);
} HalIspOps;

/** Platform-specific ISP operations (resolved at link time). */
extern HalIspOps HAL_ISP_OPS;

#ifdef __cplusplus
}
#endif
