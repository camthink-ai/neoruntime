/**
 * @file hal_audio.h
 * @brief HAL Audio - Microphone capture and audio encoding abstraction.
 *
 * Provides a unified API for audio capture and playback on edge devices:
 *   - HAL_AUDIO_TYPE_ALSA: capture/playback via ALSA (default on Linux / Hailo-15)
 *
 * Capture path (direction = HAL_AUDIO_IO_CAPTURE):
 *   1. init() -> subscribe_pcm() / subscribe_packet() -> start()
 *   2. Release buffers in callbacks via release_frame() / release_packet()
 *   3. stop() -> deinit()
 *
 * Playback path (direction = HAL_AUDIO_IO_PLAYBACK):
 *   1. init() with playback device -> start() -> write_pcm() one or more times
 *   2. stop() drains the ALSA buffer -> deinit()
 *
 * On Hailo-15: libasound for I/O; libavcodec for capture-side AAC / G.711 encode.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../common/hal_common.h"
#include "../common/hal_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_AUDIO_MAX_DEVICE_NAME_LENGTH  128
#define HAL_AUDIO_MAX_LIST_DEVICES        32

/* --------------------------------------------------------------------
 * Audio types
 * -------------------------------------------------------------------- */

/** Audio backend (ALSA). */
typedef enum {
    HAL_AUDIO_TYPE_ALSA = 0,
    HAL_AUDIO_TYPE_MAX,
} HalAudioType;

/** Capture vs speaker/line-out playback. */
typedef enum {
    HAL_AUDIO_IO_CAPTURE = 0,       /**< microphone / line-in */
    HAL_AUDIO_IO_PLAYBACK,          /**< speaker / line-out */
} HalAudioIoDirection;

/** PCM sample format for capture and optional PCM tap. */
typedef enum {
    HAL_AUDIO_SAMPLE_FMT_S16LE = 0, /**< signed 16-bit little-endian interleaved */
    HAL_AUDIO_SAMPLE_FMT_S32LE,     /**< signed 32-bit little-endian interleaved */
    HAL_AUDIO_SAMPLE_FMT_F32LE,     /**< IEEE float32 little-endian interleaved */
} HalAudioSampleFormat;

/** Encoded output codec (when packet callback is used). */
typedef enum {
    HAL_AUDIO_CODEC_PCM = 0,        /**< no encoder; only PCM callback if subscribed */
    HAL_AUDIO_CODEC_AAC,            /**< AAC-LC (ADTS) */
    HAL_AUDIO_CODEC_G711A,          /**< G.711 A-law */
    HAL_AUDIO_CODEC_G711U,          /**< G.711 mu-law */
} HalAudioCodecType;

/* --------------------------------------------------------------------
 * Audio buffers
 * -------------------------------------------------------------------- */

/**
 * PCM audio frame delivered to HalAudioFrameCallback.
 *
 * Ownership: obtained in subscribe callback; caller must release via
 * HAL_AUDIO_OPS.release_frame().
 */
typedef struct {
    uint32_t            sample_rate;    /**< samples per second */
    uint32_t            channels;       /**< channel count (1 = mono, 2 = stereo) */
    HalAudioSampleFormat format;        /**< sample format */
    uint32_t            samples;        /**< samples per channel in this buffer */
    uint64_t            timestamp_ns;   /**< capture timestamp (CLOCK_MONOTONIC) */
    uint32_t            sequence;       /**< monotonic sequence number */

    HalMemoryType       mem_type;       /**< typically HAL_MEM_MALLOC */
    uint8_t            *data;           /**< interleaved PCM payload */
    uint32_t            size;           /**< data size in bytes */

    void               *priv;           /**< platform-private (ref-count / free hook) */
} HalAudioBuffer;

/* --------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------- */

/**
 * Audio device configuration.
 */
typedef struct {
    HalAudioType         type;
    HalAudioIoDirection  direction;     /**< capture or playback; default CAPTURE */
    char                 device[HAL_AUDIO_MAX_DEVICE_NAME_LENGTH]; /**< ALSA device name */

    uint32_t             sample_rate;   /**< e.g. 8000, 16000, 48000; 0 = platform default (48000) */
    uint32_t             channels;      /**< 1 or 2; 0 = platform default (1) */
    HalAudioSampleFormat sample_format; /**< capture PCM format */
    HalAudioCodecType    codec;         /**< encoded output codec; PCM = capture-only encode path off */

    uint32_t             bitrate;       /**< encoded bitrate in bps (AAC); 0 = platform default */
    uint32_t             period_frames; /**< ALSA period size in frames; 0 = platform default (1024) */

    float                volume;        /**< software gain [0.0 .. 2.0], 1.0 = unity */
    bool                 mute;          /**< when true, silence output (callbacks may still run) */

    void                *priv;          /**< platform-specific extension (opaque) */
} HalAudioConfig;

/* --------------------------------------------------------------------
 * Callbacks
 * -------------------------------------------------------------------- */

typedef void (*HalAudioFrameCallback)(void *audio_ctx, HalAudioBuffer *frame, void *userdata);
typedef void (*HalAudioPacketCallback)(void *audio_ctx, HalPacketBuffer *packet, void *userdata);

/* --------------------------------------------------------------------
 * Device enumeration
 * -------------------------------------------------------------------- */

/** One ALSA capture device entry from list_devices(). */
typedef struct {
    char name[HAL_AUDIO_MAX_DEVICE_NAME_LENGTH];  /**< open string e.g. "hw:0,0" */
    char description[256];                        /**< human-readable label */
} HalAudioDeviceInfo;

/* --------------------------------------------------------------------
 * Operations table
 * -------------------------------------------------------------------- */

typedef struct {
    int (*init)(const HalAudioConfig *config, void **audio_ctx_return);
    int (*deinit)(void *audio_ctx);
    int (*start)(void *audio_ctx);
    int (*stop)(void *audio_ctx);

    int (*get_status)(void *audio_ctx);
    int (*get_current_config)(void *audio_ctx, HalAudioConfig *config);

    int (*subscribe_pcm)(void *audio_ctx, HalAudioFrameCallback callback, void *userdata);
    int (*unsubscribe_pcm)(void *audio_ctx, HalAudioFrameCallback callback);

    int (*subscribe_packet)(void *audio_ctx, HalAudioPacketCallback callback, void *userdata);
    int (*unsubscribe_packet)(void *audio_ctx, HalAudioPacketCallback callback);

    int (*release_frame)(void *audio_ctx, HalAudioBuffer *frame);
    int (*release_packet)(void *audio_ctx, HalPacketBuffer *packet);

    /** @brief List ALSA PCM capture (input) devices. */
    int (*list_devices)(HalAudioDeviceInfo *devices, uint32_t max_devices, uint32_t *count_out);

    /** @brief List ALSA PCM playback (output) devices. */
    int (*list_playback_devices)(HalAudioDeviceInfo *devices, uint32_t max_devices, uint32_t *count_out);

    /**
     * @brief Write one PCM buffer to a playback context (blocking).
     *
     * Context must be initialized with direction = HAL_AUDIO_IO_PLAYBACK and started.
     * @p frame must match the context sample_rate, channels, and sample_format.
     */
    int (*write_pcm)(void *audio_ctx, const HalAudioBuffer *frame);

    /**
     * @brief Runtime volume / mute / bitrate update.
     *
     * Only fields that differ from the current config are applied.
     * Bitrate changes may require encoder restart on some platforms.
     */
    int (*dynamic_change_config)(void *audio_ctx, const HalAudioConfig *config);

    const char *(*get_version)(void);
} HalAudioOps;

/** Platform-specific audio operations (resolved at link time). */
extern HalAudioOps HAL_AUDIO_OPS;

/**
 * @brief Return bytes per sample for the given format (all channels interleaved).
 */
uint32_t hal_audio_sample_format_bytes(HalAudioSampleFormat fmt);

/**
 * @brief Human-readable sample format name.
 */
const char *hal_audio_sample_format_to_string(HalAudioSampleFormat fmt);

#ifdef __cplusplus
}
#endif
