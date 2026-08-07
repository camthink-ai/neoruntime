/**
 * @file hal_buffer.h
 * @brief HAL Buffer - Unified frame and packet buffer definitions.
 *
 * Provides pixel format, memory type, and buffer descriptors shared
 * between video capture, codec, and ISP modules.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------
 * Pixel formats
 * -------------------------------------------------------------------- */
typedef enum {
    HAL_PIX_FMT_NV12 = 0,      /* YUV420 semi-planar (Y + interleaved UV) */
    HAL_PIX_FMT_NV21,           /* YUV420 semi-planar (Y + interleaved VU) */
    HAL_PIX_FMT_YUV420P,        /* YUV420 planar (Y + U + V separate planes) */
    HAL_PIX_FMT_YUYV,           /* YUV422 packed (4 bytes = 2 pixels) */
    HAL_PIX_FMT_RGB24,          /* packed RGB, 3 bytes per pixel */
    HAL_PIX_FMT_BGR24,          /* packed BGR, 3 bytes per pixel */
    HAL_PIX_FMT_ARGB32,         /* packed ARGB, 4 bytes per pixel */
    HAL_PIX_FMT_RGBA32,         /* packed RGBA, 4 bytes per pixel */
    HAL_PIX_FMT_GRAY8,          /* 8-bit grayscale */
    HAL_PIX_FMT_RAW10,          /* 10-bit raw Bayer */
    HAL_PIX_FMT_RAW12,          /* 12-bit raw Bayer */
} HalPixelFormat;

/* --------------------------------------------------------------------
 * Memory types
 * -------------------------------------------------------------------- */
typedef enum {
    HAL_MEM_DMABUF = 0,         /* DMA-BUF file descriptor (zero-copy) */
    HAL_MEM_MMAP,               /* memory-mapped (V4L2 MMAP style) */
    HAL_MEM_MALLOC,             /* standard heap allocation */
} HalMemoryType;

/* --------------------------------------------------------------------
 * Frame buffer (raw video frames)
 * -------------------------------------------------------------------- */
#define HAL_MAX_PLANES 3

/**
 * Unified frame buffer descriptor exchanged between HAL modules.
 *
 * Ownership rules:
 * - Obtained via read_frame / subscribe callback: caller must release
 *   via release_frame.
 * - ref / release provide reference counting for shared access.
 * - The priv field is managed by the platform adapter; callers must
 *   not modify it.
 */
typedef struct {
    /* metadata */
    uint32_t        width;                          /* image width in pixels */
    uint32_t        height;                         /* image height in pixels */
    HalPixelFormat  format;                         /* pixel format */
    HalMemoryType   mem_type;                       /* how the memory was allocated */
    uint32_t        sequence;                       /* monotonic frame sequence number */
    uint64_t        timestamp_ns;                   /* capture timestamp (nanoseconds, CLOCK_MONOTONIC) */

    /* plane data (multi-planar: NV12 = 2 planes, YUV420P = 3 planes, packed = 1 plane) */
    uint32_t        num_planes;                     /* number of valid planes [1..HAL_MAX_PLANES] */
    int             dma_fds[HAL_MAX_PLANES];        /* DMA-BUF file descriptors, -1 if not applicable */
    void           *planes[HAL_MAX_PLANES];         /* user-space virtual addresses (may be NULL for DMABUF-only) */
    uint32_t        strides[HAL_MAX_PLANES];        /* bytes per row including padding */
    uint32_t        sizes[HAL_MAX_PLANES];          /* total allocation size per plane in bytes */

    void           *metadata;                       /* platform-specific metadata (opaque) */
    void           *priv;                           /* platform-private data (ref-counted internally, do not touch) */
} HalFrameBuffer;

/* --------------------------------------------------------------------
 * Frame buffer allocation / request interface
 * -------------------------------------------------------------------- */

/**
 * @brief Frame buffer request parameters.
 *
 * Ownership rules:
 * - Obtained via HAL_FRAME_BUFFER_OPS.request_frame_buffer()
 * - Released via HAL_FRAME_BUFFER_OPS.release_frame_buffer()
 */
typedef struct {
    uint32_t        width;                          /* output frame width in pixels */
    uint32_t        height;                         /* output frame height in pixels */
    HalPixelFormat  format;                         /* desired pixel format */
    uint32_t        pool_max_buffers;               /* max buffers in the frame pool (0 = platform default) */
    HalMemoryType   mem_type;                       /* desired memory type (platform may restrict) */
    bool            zero_initialize;               /* if true, plane memory will be zeroed */
    void           *priv;                           /* platform-specific extension (opaque) */
} HalFrameBufferRequest;

/**
 * @brief Platform operations for requesting/releasing HalFrameBuffer.
 *
 * Platform implementations must define HAL_FRAME_BUFFER_OPS at link time.
 */
typedef struct {
    int (*request_frame_buffer)(const HalFrameBufferRequest *req, HalFrameBuffer **frame_out);
    int (*copy_metadata_from_frame_buffer)(const HalFrameBuffer *src, HalFrameBuffer *dst);
    int (*release_frame_buffer)(HalFrameBuffer *frame);
    const char *(*get_version)(void);
} HalFrameBufferOps;

/** Platform-specific frame buffer operations (resolved at link time). */
extern HalFrameBufferOps HAL_FRAME_BUFFER_OPS;

/* --------------------------------------------------------------------
 * Packet buffer (encoded bitstream)
 * -------------------------------------------------------------------- */

/** Encoded packet type (codec output format). */
typedef enum {
    HAL_PACKET_TYPE_H264 = 0,   /* H.264 / AVC NAL units */
    HAL_PACKET_TYPE_H265,       /* H.265 / HEVC NAL units */
    HAL_PACKET_TYPE_MJPEG,      /* Motion JPEG frame */
    HAL_PACKET_TYPE_AAC,        /* AAC access units (typically ADTS) */
    HAL_PACKET_TYPE_G711A,      /* G.711 A-law payload */
    HAL_PACKET_TYPE_G711U,      /* G.711 mu-law payload */
    HAL_PACKET_TYPE_PCM,        /* raw PCM blob (passthrough / debug) */
    HAL_PACKET_TYPE_DATA,       /* opaque data blob */
    HAL_PACKET_TYPE_MAX,        /* sentinel */
} HalPacketType;

/**
 * Encoded bitstream packet descriptor.
 *
 * Ownership: obtained via subscribe callback or read_packet; caller must
 * release via release_packet when done.
 */
typedef struct {
    HalPacketType   type;           /* encoded format */
    HalMemoryType   mem_type;       /* how the memory was allocated */

    int             dma_fd;         /* DMA-BUF fd, -1 if not applicable */
    uint8_t        *data;           /* pointer to encoded data (user-space) */
    uint32_t        size;           /* encoded data size in bytes */

    uint32_t        sequence;       /* monotonic packet sequence number */
    uint64_t        timestamp_ns;   /* capture timestamp in nanoseconds */

    void           *metadata;       /* platform-specific metadata (opaque) */
    void           *priv;           /* platform-private data (ref-counted internally, do not touch) */
} HalPacketBuffer;

/* --------------------------------------------------------------------
 * Pixel format helpers
 * -------------------------------------------------------------------- */

/**
 * @brief Return the number of image planes for the given pixel format.
 * @param fmt Pixel format.
 * @return Number of planes (1, 2, or 3), or 0 for unknown formats.
 */
uint32_t hal_pixel_format_plane_count(HalPixelFormat fmt);

/**
 * @brief Return a human-readable string for the pixel format (e.g. "NV12").
 * @param fmt Pixel format.
 * @return Static string (never NULL).
 */
const char *hal_pixel_format_to_string(HalPixelFormat fmt);

#ifdef __cplusplus
}
#endif
