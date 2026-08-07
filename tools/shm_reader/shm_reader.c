/**
 * @file shm_reader.c
 * @brief SHM Ring Buffer Reader - Test tool for verifying camera-daemon SHM output
 *
 * Reads frames from the SHM ring buffer created by camera-daemon's ShmPublisher.
 * Reports frame metadata, sequence gaps, latency, and optionally dumps raw data.
 *
 * Usage:
 *   shm_reader /run/aipc/shm/main.shm        # Read from main stream
 *   shm_reader /run/aipc/shm/ai.shm -n 100   # Read 100 frames then exit
 *   shm_reader /run/aipc/shm/sub.shm -d       # Dump first 64 bytes of each frame
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>
#include <stdatomic.h>

/* Inline SHM protocol definitions (must match shm_protocol.h) */
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

    uint8_t reserved[SHM_HEADER_SIZE - 0x48];
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

/* ========== Globals ========== */
static volatile sig_atomic_t g_running = 1;
static void sig_handler(int s) { (void)s; g_running = 0; }

static const char* fmt_name(uint32_t f) {
    switch (f) {
    case 0: return "NV12";
    case 1: return "NV21";
    case 2: return "YUYV";
    case 3: return "RGB24";
    case 4: return "BGR24";
    case 5: return "RGBA";
    default: return "UNKNOWN";
    }
}

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s <shm_path> [options]\n"
                    "  -n, --count N     Read N frames then exit (default: unlimited)\n"
                    "  -d, --dump        Dump first 64 bytes of pixel data\n"
                    "  -s, --stats       Print periodic stats (every 30 frames)\n"
                    "  -h, --help        Show this help\n", prog);
}

int main(int argc, char** argv) {
    const char* shm_path = NULL;
    int max_frames = 0;
    bool dump_data = false;
    bool show_stats = false;

    static struct option long_opts[] = {
        {"count", required_argument, NULL, 'n'},
        {"dump",  no_argument,       NULL, 'd'},
        {"stats", no_argument,       NULL, 's'},
        {"help",  no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "n:dsh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'n': max_frames = atoi(optarg); break;
        case 'd': dump_data = true; break;
        case 's': show_stats = true; break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: SHM path required\n");
        print_usage(argv[0]);
        return 1;
    }
    shm_path = argv[optind];

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* Open SHM file */
    int fd = open(shm_path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        fprintf(stderr, "Cannot open SHM file: %s\n", shm_path);
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return 1;
    }

    void* mapped = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    close(fd);

    ShmHeader* hdr = (ShmHeader*)mapped;

    /* Validate header */
    if (hdr->magic != SHM_MAGIC) {
        fprintf(stderr, "Error: Invalid SHM magic: 0x%08X (expected 0x%08X)\n",
                hdr->magic, SHM_MAGIC);
        munmap(mapped, st.st_size);
        return 1;
    }

    printf("=== SHM Ring Buffer Reader ===\n");
    printf("File:     %s\n", shm_path);
    printf("Version:  %u\n", hdr->version);
    printf("Size:     %ux%u @ %u fps\n", hdr->width, hdr->height, hdr->fps);
    printf("Format:   %s (%u)\n", fmt_name(hdr->format), hdr->format);
    printf("Planes:   %u  strides=[%u, %u, %u]\n",
           hdr->num_planes, hdr->strides[0], hdr->strides[1], hdr->strides[2]);
    printf("Buffers:  %u  slot_size=%u  data_offset=%u\n",
           hdr->buffer_count, hdr->slot_size, hdr->data_offset);
    printf("File size: %ld bytes\n\n", (long)st.st_size);

    uint64_t last_seq = 0;
    int frames_read = 0;
    uint64_t total_gaps = 0;
    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    uint64_t prev_write_seq = atomic_load_explicit(&hdr->write_seq, memory_order_acquire);

    while (g_running && (max_frames == 0 || frames_read < max_frames)) {
        /* Spin-wait for new write */
        uint64_t cur_write_seq = atomic_load_explicit(&hdr->write_seq, memory_order_acquire);
        if (cur_write_seq == prev_write_seq) {
            usleep(1000);  /* 1ms poll */
            continue;
        }
        prev_write_seq = cur_write_seq;

        /* Read latest slot */
        uint64_t slot_idx = atomic_load_explicit(&hdr->latest_slot, memory_order_acquire);
        if (slot_idx >= hdr->buffer_count) continue;

        uint8_t* slot_ptr = (uint8_t*)mapped + SHM_HEADER_SIZE
                           + slot_idx * hdr->slot_size;
        ShmSlotHeader* slot = (ShmSlotHeader*)slot_ptr;

        uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
        if (state != SHM_SLOT_READY) continue;

        /* Frame info */
        uint64_t seq = slot->sequence;
        uint64_t gap = 0;
        if (frames_read > 0 && seq > last_seq + 1) {
            gap = seq - last_seq - 1;
            total_gaps += gap;
        }

        struct timespec t_now;
        clock_gettime(CLOCK_MONOTONIC, &t_now);
        uint64_t now_ns = (uint64_t)t_now.tv_sec * 1000000000ULL + t_now.tv_nsec;
        double latency_ms = (double)(now_ns - slot->timestamp_ns) / 1e6;

        printf("[%06d] seq=%lu slot=%lu %ux%u %s data=%u lat=%.2fms",
               frames_read, (unsigned long)seq, (unsigned long)slot_idx,
               slot->width, slot->height,
               fmt_name(slot->format), slot->data_size, latency_ms);

        if (gap > 0) {
            printf(" GAP=%lu", (unsigned long)gap);
        }
        printf("\n");

        /* Optionally dump first bytes of pixel data */
        if (dump_data && slot->data_size > 0) {
            uint8_t* data = slot_ptr + SHM_SLOT_HDR_SIZE;
            int dump_len = slot->data_size < 64 ? slot->data_size : 64;
            printf("  Data: ");
            for (int i = 0; i < dump_len; i++) {
                printf("%02x", data[i]);
                if ((i & 15) == 15) printf("\n        ");
                else printf(" ");
            }
            printf("\n");
        }

        last_seq = seq;
        frames_read++;

        /* Periodic stats */
        if (show_stats && frames_read > 0 && frames_read % 30 == 0) {
            uint64_t elapsed_ns = now_ns
                - ((uint64_t)t_start.tv_sec * 1000000000ULL + t_start.tv_nsec);
            double elapsed_s = (double)elapsed_ns / 1e9;
            double fps = frames_read / elapsed_s;
            printf("  --- Stats: %d frames in %.1fs = %.1f fps, gaps=%lu ---\n",
                   frames_read, elapsed_s, fps, (unsigned long)total_gaps);
        }
    }

    /* Summary */
    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double total_s = (double)((uint64_t)t_end.tv_sec * 1000000000ULL + t_end.tv_nsec
                    - (uint64_t)t_start.tv_sec * 1000000000ULL - t_start.tv_nsec) / 1e9;

    printf("\n=== Summary ===\n");
    printf("Frames read: %d\n", frames_read);
    printf("Duration:    %.2f s\n", total_s);
    if (total_s > 0) {
        printf("Avg FPS:     %.1f\n", frames_read / total_s);
    }
    printf("Seq gaps:    %lu\n", (unsigned long)total_gaps);

    munmap(mapped, st.st_size);
    return 0;
}
