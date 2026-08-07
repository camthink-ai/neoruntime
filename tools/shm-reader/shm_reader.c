/**
 * @file shm_reader.c
 * @brief SHM Ring Buffer Reader - Test tool for reading video frames from camera-daemon SHM
 *
 * Usage:
 *   shm_reader /run/aipc/shm/stream0.raw              # Print frame info
 *   shm_reader /run/aipc/shm/stream0.raw --dump 10    # Dump first 10 frames as NV12 files
 *   shm_reader /run/aipc/shm/stream0.raw --fps         # Measure frame rate
 *   shm_reader /run/aipc/shm/stream0.raw --latency     # Measure delivery latency
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* ========== Inline SHM protocol (matching shm_protocol.h) ========== */

#define SHM_MAGIC           0x41495043
#define SHM_VERSION         2
#define SHM_HEADER_SIZE     4096
#define SHM_SLOT_HDR_SIZE   64
#define SHM_MAX_PLANES      3

#define SHM_SLOT_EMPTY      0
#define SHM_SLOT_WRITING    1
#define SHM_SLOT_READY      2

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t fps;
    uint32_t num_planes;
    uint32_t strides[SHM_MAX_PLANES];
    uint32_t buffer_count;
    uint32_t slot_size;
    uint32_t data_offset;
    uint32_t _pad0;
    _Atomic uint64_t write_seq;
    _Atomic uint64_t latest_slot;
    uint8_t  reserved[SHM_HEADER_SIZE - 0x48];
} ShmHeader;

typedef struct {
    uint64_t sequence;
    uint64_t timestamp_ns;
    uint32_t data_size;
    _Atomic uint32_t state;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t num_planes;
    uint32_t plane_offsets[SHM_MAX_PLANES];
    uint32_t plane_sizes[SHM_MAX_PLANES];
} ShmSlotHeader;

static inline void* shm_get_slot(void* base, uint32_t i, uint32_t slot_size) {
    return (uint8_t*)base + SHM_HEADER_SIZE + (uint64_t)i * slot_size;
}

static inline void* shm_slot_data(void* slot) {
    return (uint8_t*)slot + SHM_SLOT_HDR_SIZE;
}

/* ========== Globals ========== */

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static const char* format_name(uint32_t fmt) {
    switch (fmt) {
        case 0: return "NV12";
        case 1: return "YUV420P";
        case 2: return "YUYV";
        case 3: return "RGB24";
        case 4: return "RGBA";
        case 5: return "GRAY8";
        default: return "UNKNOWN";
    }
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ========== Commands ========== */

static void print_header(const ShmHeader* hdr) {
    printf("=== SHM Ring Buffer ===\n");
    printf("  Magic:        0x%08X %s\n", hdr->magic,
           hdr->magic == SHM_MAGIC ? "(OK)" : "(INVALID!)");
    printf("  Version:      %u\n", hdr->version);
    printf("  Resolution:   %ux%u\n", hdr->width, hdr->height);
    printf("  Format:       %s (%u)\n", format_name(hdr->format), hdr->format);
    printf("  FPS:          %u\n", hdr->fps);
    printf("  Planes:       %u\n", hdr->num_planes);
    for (uint32_t i = 0; i < hdr->num_planes && i < SHM_MAX_PLANES; i++) {
        printf("  Stride[%u]:    %u\n", i, hdr->strides[i]);
    }
    printf("  Buffers:      %u\n", hdr->buffer_count);
    printf("  Slot size:    %u bytes\n", hdr->slot_size);
    printf("  Data offset:  %u\n", hdr->data_offset);
    printf("  Write seq:    %lu\n",
           (unsigned long)atomic_load_explicit(&hdr->write_seq, memory_order_acquire));
    printf("  Latest slot:  %lu\n",
           (unsigned long)atomic_load_explicit(&hdr->latest_slot, memory_order_acquire));
    printf("========================\n");
}

static void print_slot(const ShmSlotHeader* slot, uint32_t idx) {
    uint32_t st = atomic_load_explicit(&slot->state, memory_order_acquire);
    const char* state_str = (st == SHM_SLOT_EMPTY) ? "EMPTY" :
                            (st == SHM_SLOT_WRITING) ? "WRITING" :
                            (st == SHM_SLOT_READY) ? "READY" : "???";

    printf("  Slot[%u]: state=%s seq=%lu ts=%lu size=%u %ux%u fmt=%s planes=%u\n",
           idx, state_str,
           (unsigned long)slot->sequence,
           (unsigned long)slot->timestamp_ns,
           slot->data_size,
           slot->width, slot->height,
           format_name(slot->format),
           slot->num_planes);

    for (uint32_t p = 0; p < slot->num_planes && p < SHM_MAX_PLANES; p++) {
        printf("    plane[%u]: offset=%u size=%u\n",
               p, slot->plane_offsets[p], slot->plane_sizes[p]);
    }
}

/**
 * Default mode: print header + poll for new frames, print info
 */
static int cmd_watch(void* base, const ShmHeader* hdr) {
    print_header(hdr);

    uint64_t last_seq = atomic_load_explicit(&hdr->write_seq, memory_order_acquire);
    printf("\nWaiting for frames (Ctrl+C to stop)...\n\n");

    uint64_t frames_seen = 0;
    uint64_t first_ts = 0;

    while (g_running) {
        uint64_t seq = atomic_load_explicit(&hdr->write_seq, memory_order_acquire);
        if (seq == last_seq) {
            usleep(1000);  /* 1ms poll */
            continue;
        }

        uint64_t slot_idx = atomic_load_explicit(&hdr->latest_slot, memory_order_acquire);
        if (slot_idx >= hdr->buffer_count) {
            last_seq = seq;
            continue;
        }

        ShmSlotHeader* slot = (ShmSlotHeader*)shm_get_slot(base,
                                (uint32_t)slot_idx, hdr->slot_size);
        uint32_t st = atomic_load_explicit(&slot->state, memory_order_acquire);
        if (st != SHM_SLOT_READY) {
            last_seq = seq;
            continue;
        }

        frames_seen++;
        if (first_ts == 0) first_ts = now_ns();

        printf("[seq=%lu] slot=%lu %ux%u %s size=%u ts=%lu ns",
               (unsigned long)seq,
               (unsigned long)slot_idx,
               slot->width, slot->height,
               format_name(slot->format),
               slot->data_size,
               (unsigned long)slot->timestamp_ns);

        if (frames_seen > 1) {
            uint64_t elapsed = now_ns() - first_ts;
            double fps = (double)(frames_seen - 1) * 1e9 / elapsed;
            printf("  avg_fps=%.1f", fps);
        }
        printf("\n");

        last_seq = seq;
    }

    printf("\nTotal frames seen: %lu\n", (unsigned long)frames_seen);
    return 0;
}

/**
 * --dump N: save first N frames as NV12 files
 */
static int cmd_dump(void* base, const ShmHeader* hdr, int max_frames, const char* out_dir) {
    print_header(hdr);

    uint64_t last_seq = atomic_load_explicit(&hdr->write_seq, memory_order_acquire);
    printf("Dumping up to %d frames to %s/ ...\n", max_frames, out_dir);

    int dumped = 0;

    while (g_running && dumped < max_frames) {
        uint64_t seq = atomic_load_explicit(&hdr->write_seq, memory_order_acquire);
        if (seq == last_seq) {
            usleep(1000);
            continue;
        }

        uint64_t slot_idx = atomic_load_explicit(&hdr->latest_slot, memory_order_acquire);
        if (slot_idx >= hdr->buffer_count) { last_seq = seq; continue; }

        ShmSlotHeader* slot = (ShmSlotHeader*)shm_get_slot(base,
                                (uint32_t)slot_idx, hdr->slot_size);
        uint32_t st = atomic_load_explicit(&slot->state, memory_order_acquire);
        if (st != SHM_SLOT_READY) { last_seq = seq; continue; }

        void* data = shm_slot_data(slot);

        char path[512];
        snprintf(path, sizeof(path), "%s/frame_%06d_%ux%u.nv12",
                 out_dir, dumped, slot->width, slot->height);

        FILE* fp = fopen(path, "wb");
        if (fp) {
            fwrite(data, 1, slot->data_size, fp);
            fclose(fp);
            printf("  [%d] seq=%lu -> %s (%u bytes)\n",
                   dumped, (unsigned long)slot->sequence, path, slot->data_size);
        } else {
            fprintf(stderr, "  Failed to open %s: %s\n", path, strerror(errno));
        }

        dumped++;
        last_seq = seq;
    }

    printf("Dumped %d frames\n", dumped);
    return 0;
}

/**
 * --fps: measure frame rate over time
 */
static int cmd_fps(void* base, const ShmHeader* hdr) {
    print_header(hdr);

    printf("Measuring FPS (Ctrl+C to stop)...\n\n");

    uint64_t last_seq = atomic_load_explicit(&hdr->write_seq, memory_order_acquire);
    uint64_t interval_start = now_ns();
    uint64_t interval_frames = 0;
    uint64_t total_frames = 0;
    uint64_t total_start = now_ns();

    while (g_running) {
        uint64_t seq = atomic_load_explicit(&hdr->write_seq, memory_order_acquire);
        if (seq == last_seq) {
            usleep(500);
            continue;
        }

        uint64_t delta = seq - last_seq;
        interval_frames += delta;
        total_frames += delta;
        last_seq = seq;

        uint64_t elapsed = now_ns() - interval_start;
        if (elapsed >= 1000000000ULL) {  /* 1 second */
            double fps = (double)interval_frames * 1e9 / elapsed;
            double avg = (double)total_frames * 1e9 / (now_ns() - total_start);
            printf("  FPS: %.1f (avg: %.1f) total_frames: %lu\n",
                   fps, avg, (unsigned long)total_frames);
            interval_frames = 0;
            interval_start = now_ns();
        }
    }

    double avg = (total_frames > 0)
        ? (double)total_frames * 1e9 / (now_ns() - total_start) : 0;
    printf("\nTotal: %lu frames, avg FPS: %.1f\n",
           (unsigned long)total_frames, avg);
    return 0;
}

/**
 * --latency: measure time between frame timestamp and reader receive time
 */
static int cmd_latency(void* base, const ShmHeader* hdr) {
    print_header(hdr);

    printf("Measuring latency (Ctrl+C to stop)...\n");
    printf("  NOTE: requires daemon & reader to share CLOCK_MONOTONIC\n\n");

    uint64_t last_seq = atomic_load_explicit(&hdr->write_seq, memory_order_acquire);
    uint64_t count = 0;
    double sum_us = 0;
    double min_us = 1e12;
    double max_us = 0;

    while (g_running) {
        uint64_t seq = atomic_load_explicit(&hdr->write_seq, memory_order_acquire);
        if (seq == last_seq) {
            usleep(500);
            continue;
        }

        uint64_t read_ts = now_ns();
        uint64_t slot_idx = atomic_load_explicit(&hdr->latest_slot, memory_order_acquire);
        if (slot_idx >= hdr->buffer_count) { last_seq = seq; continue; }

        ShmSlotHeader* slot = (ShmSlotHeader*)shm_get_slot(base,
                                (uint32_t)slot_idx, hdr->slot_size);
        uint32_t st = atomic_load_explicit(&slot->state, memory_order_acquire);
        if (st != SHM_SLOT_READY) { last_seq = seq; continue; }

        /* Latency = reader_time - frame_timestamp */
        if (slot->timestamp_ns > 0 && read_ts > slot->timestamp_ns) {
            double lat_us = (double)(read_ts - slot->timestamp_ns) / 1000.0;
            sum_us += lat_us;
            if (lat_us < min_us) min_us = lat_us;
            if (lat_us > max_us) max_us = lat_us;
            count++;

            if (count % 30 == 0) {
                printf("  [%lu frames] lat: min=%.1f avg=%.1f max=%.1f us\n",
                       (unsigned long)count, min_us, sum_us / count, max_us);
            }
        }

        last_seq = seq;
    }

    if (count > 0) {
        printf("\nLatency: min=%.1f avg=%.1f max=%.1f us (%lu frames)\n",
               min_us, sum_us / count, max_us, (unsigned long)count);
    }
    return 0;
}

/**
 * --info: print header and all slot states, then exit
 */
static int cmd_info(void* base, const ShmHeader* hdr) {
    print_header(hdr);

    printf("\nSlot states:\n");
    for (uint32_t i = 0; i < hdr->buffer_count; i++) {
        ShmSlotHeader* slot = (ShmSlotHeader*)shm_get_slot(base, i, hdr->slot_size);
        print_slot(slot, i);
    }
    return 0;
}

/* ========== Main ========== */

static void usage(const char* prog) {
    fprintf(stderr, "Usage: %s <shm_file> [options]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  (none)           Watch frames in real-time\n");
    fprintf(stderr, "  --info           Print header + all slot states and exit\n");
    fprintf(stderr, "  --fps            Measure frame rate\n");
    fprintf(stderr, "  --latency        Measure delivery latency\n");
    fprintf(stderr, "  --dump N [DIR]   Dump N frames as .nv12 files (default dir: .)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s /run/aipc/shm/stream0.raw\n", prog);
    fprintf(stderr, "  %s /run/aipc/shm/stream0.raw --dump 10 /tmp/frames\n", prog);
    fprintf(stderr, "  %s /run/aipc/shm/stream0.raw --fps\n", prog);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char* shm_path = argv[1];

    /* Parse mode */
    enum { MODE_WATCH, MODE_INFO, MODE_FPS, MODE_LATENCY, MODE_DUMP } mode = MODE_WATCH;
    int dump_count = 10;
    const char* dump_dir = ".";

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--info") == 0) {
            mode = MODE_INFO;
        } else if (strcmp(argv[i], "--fps") == 0) {
            mode = MODE_FPS;
        } else if (strcmp(argv[i], "--latency") == 0) {
            mode = MODE_LATENCY;
        } else if (strcmp(argv[i], "--dump") == 0) {
            mode = MODE_DUMP;
            if (i + 1 < argc) dump_count = atoi(argv[++i]);
            if (i + 1 < argc && argv[i+1][0] != '-') dump_dir = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Open SHM file */
    int fd = open(shm_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", shm_path, strerror(errno));
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < SHM_HEADER_SIZE) {
        fprintf(stderr, "File too small or fstat failed: %s\n", shm_path);
        close(fd);
        return 1;
    }

    void* base = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    const ShmHeader* hdr = (const ShmHeader*)base;

    /* Validate */
    if (hdr->magic != SHM_MAGIC) {
        fprintf(stderr, "Invalid SHM magic: 0x%08X (expected 0x%08X)\n",
                hdr->magic, SHM_MAGIC);
        munmap(base, st.st_size);
        close(fd);
        return 1;
    }

    if (hdr->version != SHM_VERSION) {
        fprintf(stderr, "Warning: SHM version %u (expected %u)\n",
                hdr->version, SHM_VERSION);
    }

    int ret = 0;
    switch (mode) {
        case MODE_WATCH:   ret = cmd_watch(base, hdr); break;
        case MODE_INFO:    ret = cmd_info(base, hdr); break;
        case MODE_FPS:     ret = cmd_fps(base, hdr); break;
        case MODE_LATENCY: ret = cmd_latency(base, hdr); break;
        case MODE_DUMP:    ret = cmd_dump(base, hdr, dump_count, dump_dir); break;
    }

    munmap(base, st.st_size);
    close(fd);
    return ret;
}
