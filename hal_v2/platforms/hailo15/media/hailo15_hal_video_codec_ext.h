/**
 * @file hailo15_hal_video_codec_ext.h
 * @brief Hailo-15 extensions for HalVideoConfig.priv / HalCodecConfig.priv (CSI / HW codec).
 *
 * Portable headers (hal_video.h / hal_codec.h) stay unchanged; applications pass
 * a pointer to these structs via the opaque `priv` field when using HAL_VIDEO_TYPE_CSI
 * or HAL_CODEC_TYPE_HW on Hailo-15.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** CSI / standalone video: which Media Library stack backs HAL_VIDEO_TYPE_CSI. */
typedef enum {
    /** MediaLibraryFrontend only (frontend JSON schema). */
    HAILO15_CSI_PIPELINE_FRONTEND_ONLY = 0,
    /** Internal MediaLibrary::initialize (full profile / ISP path). */
    HAILO15_CSI_PIPELINE_MEDIALIB_FULL = 1,
} Hailo15CsiPipelineMode;

/**
 * Optional configuration for HAL_VIDEO_TYPE_CSI (point HalVideoConfig.priv here).
 *
 * Resolution priority (V1-aligned): medialib_config_json → medialib_config_path →
 * frontend_config_json → frontend_config_path → legacy_config_json → legacy_config_path.
 * Within each tier, JSON wins over path.
 *
 * **Frames vs. feeding (FRONTEND_ONLY):** HAL exposes `subscribe_stream` + `start` /
 * `stop` only. For typical board configs, the Media Library frontend JSON uses
 * `input_video.source_type` **V4L2SRC** (this is also the schema default when the field
 * is omitted, as in `doc/.../frontend_config_example.json`). In that mode the pipeline
 * pulls from the sensor / V4L2 device and **delivers frames to your callbacks**; there
 * is **no** HalVideo-level “push frame” API. Only if the JSON explicitly selects
 * **APPSRC** does the vendor `MediaLibraryFrontend::add_buffer` path apply (outside
 * portable HalVideoOps).
 *
 * When @ref csi_pipeline_mode is FRONTEND_ONLY and the resolved document looks like a
 * full medialib root (profiles + version), init fails with HAL_ERR_INVALID_ARG — use
 * HalMedia + FROM_MEDIA instead.
 */
typedef struct Hailo15HalVideoPrivExt {
    Hailo15CsiPipelineMode csi_pipeline_mode;
    const char *medialib_config_json;
    const char *medialib_config_path;
    const char *frontend_config_json;
    const char *frontend_config_path;
    const char *legacy_config_json;
    const char *legacy_config_path;
} Hailo15HalVideoPrivExt;

/**
 * Optional configuration for HAL_CODEC_TYPE_HW (point HalCodecConfig.priv here).
 *
 * Encoder JSON priority (V1-aligned): config_path → config_json → embedded default
 * patched with HalCodecConfig fields.
 */
typedef struct Hailo15HalCodecPrivExt {
    /** Passed to MediaLibraryEncoder::create(); must match JSON stream identity when applicable. */
    const char *encoder_stream_id;
    const char *config_path;
    const char *config_json;
    const char *osd_config_path;
    const char *osd_config_json;
} Hailo15HalCodecPrivExt;

#ifdef __cplusplus
}
#endif
