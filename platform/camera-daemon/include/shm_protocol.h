/**
 * @file shm_protocol.h
 * @brief SHM Ring Buffer Protocol - Shared memory layout for video frame delivery
 *
 * Memory layout:
 *   [ShmHeader (4096 B)] [Slot0] [Slot1] ... [SlotN-1]
 *   Each Slot = [ShmSlotHeader (64 B)] [Pixel Data]
 *
 * Single-writer (camera-daemon) / multi-reader (App containers) lock-free protocol.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
#include <atomic>
#define _SHM_ATOMIC(T)  std::atomic<T>
extern "C" {
#else
#include <stdatomic.h>
#define _SHM_ATOMIC(T)  _Atomic T
#endif

#define SHM_MAGIC           0x41495043  /* 'AIPC' */
#define SHM_VERSION         2
#define SHM_HEADER_SIZE     4096        /* Page-aligned */
#define SHM_SLOT_HDR_SIZE   64          /* Cache-line aligned */
#define SHM_MAX_PLANES      3

/* ========== Slot state ========== */
#define SHM_SLOT_EMPTY      0
#define SHM_SLOT_WRITING    1
#define SHM_SLOT_READY      2

/* ========== File header (fixed 4096 bytes, first page) ========== */
typedef struct {
    uint32_t magic;                         /* +0x00  Must be SHM_MAGIC */
    uint32_t version;                       /* +0x04  Protocol version */
    uint32_t width;                         /* +0x08  Frame width */
    uint32_t height;                        /* +0x0C  Frame height */
    uint32_t format;                        /* +0x10  HalPixelFormat */
    uint32_t fps;                           /* +0x14  Frame rate */
    uint32_t num_planes;                    /* +0x18  Number of planes */
    uint32_t strides[SHM_MAX_PLANES];       /* +0x1C  Row stride per plane */

    uint32_t buffer_count;                  /* +0x28  Number of slots */
    uint32_t slot_size;                     /* +0x2C  Total bytes per slot (hdr+data) */
    uint32_t data_offset;                   /* +0x30  Offset of first slot from file start */
    uint32_t _pad0;                         /* +0x34  Alignment */

    /* Lock-free synchronization (single-writer, multi-reader) */
    _SHM_ATOMIC(uint64_t) write_seq;       /* +0x38  Monotonically increasing write counter */
    _SHM_ATOMIC(uint64_t) latest_slot;     /* +0x40  Index of most recent ready slot */

    uint8_t  reserved[SHM_HEADER_SIZE - 0x48]; /* Pad to page boundary */
} ShmHeader;

/* ========== Per-slot header (64 bytes, cache-line aligned) ========== */
typedef struct {
    uint64_t sequence;                      /* +0x00  Frame sequence (from HAL) */
    uint64_t timestamp_ns;                  /* +0x08  Capture timestamp (ns) */

    uint32_t data_size;                     /* +0x10  Total valid pixel bytes */
    _SHM_ATOMIC(uint32_t) state;            /* +0x14  SHM_SLOT_EMPTY/WRITING/READY */

    uint32_t width;                         /* +0x18  Actual frame width */
    uint32_t height;                        /* +0x1C  Actual frame height */

    uint32_t format;                        /* +0x20  HalPixelFormat */
    uint32_t num_planes;                    /* +0x24  Number of planes */

    uint32_t plane_offsets[SHM_MAX_PLANES]; /* +0x28  Offset of each plane from slot data start */
    uint32_t plane_sizes[SHM_MAX_PLANES];   /* +0x34  Size of each plane in bytes */
} ShmSlotHeader;

/* ========== Inline helpers ========== */

/** Calculate NV12 frame data size */
static inline uint32_t shm_calc_nv12_size(uint32_t w, uint32_t h) {
    return w * h + w * (h / 2);
}

/** Calculate slot size (header + data, 64-byte aligned) */
static inline uint32_t shm_calc_slot_size(uint32_t data_size) {
    return SHM_SLOT_HDR_SIZE + ((data_size + 63u) & ~63u);
}

/** Calculate total SHM file size */
static inline uint64_t shm_calc_total_size(uint32_t slot_size, uint32_t buffer_count) {
    return (uint64_t)SHM_HEADER_SIZE + (uint64_t)slot_size * buffer_count;
}

/** Get pointer to slot i */
static inline void* shm_get_slot(void* base, uint32_t i, uint32_t slot_size) {
    return (uint8_t*)base + SHM_HEADER_SIZE + (uint64_t)i * slot_size;
}

/** Get pointer to pixel data within a slot */
static inline void* shm_slot_data(void* slot) {
    return (uint8_t*)slot + SHM_SLOT_HDR_SIZE;
}

#ifdef __cplusplus
}
#endif
