/**
 * @file dma_bind_ab_test.cpp
 * @brief P1-1 functional A/B: tensor_from_frame DMA direct-bind vs CPU memcpy.
 *
 * Fills compact dual-plane NV12 dmabufs (dma_heap, one fd per plane — the
 * exact layout MediaLibraryBufferPool gives HAL-requested frames) with a
 * frame-salted deterministic pattern, then pushes the same content through:
 *   A) HalFrameBuffer{dma_fds set, compact strides} -> P1-1 fast path
 *      (tensor.data==NULL, dma_fd>=0, set_pix_buffer DMABUF at bind time)
 *   B) HalFrameBuffer{CPU planes only}              -> legacy memcpy staging
 * and byte-compares every output tensor (FNV-1a over the full buffer).
 * Fresh content every iteration also proves the NPU reads the CURRENT
 * dmabuf contents per submit — a stale binding would break parity from
 * iteration 2 onward.
 *
 * Also checks the mismatch contract: a DMA frame whose declared geometry
 * differs from the model input must fail cleanly at run() with
 * HAL_ERR_INVALID_SIZE (the bind-time byte_size == frame_size check).
 *
 * Usage: hal-dma-bind-ab-test <model.hef> [iters=20] [w h]
 * (w/h optional overrides; default derived from the model's first input,
 *  which must be NV12 — shape[1]=luma height, shape[2]=width.)
 */

#include "common/hal_buffer.h"
#include "common/hal_common.h"
#include "model/hal_inference.h"

#include <linux/dma-heap.h>
#include <linux/dma-buf.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace
{

/* ------------------------------------------------------------- hashing -- */

static uint64_t fnv1a(const void *data, size_t len)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++)
    {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* --------------------------------------------------------- dma-heap i/f -- */

static const char *const kHeapCandidates[] = {
    /* rig 93.72 (hailo15) heaps, verified 2026-09-15 — same list as
     * tools/npu-bind-probe */
    "/dev/dma_heap/linux,cma",
    "/dev/dma_heap/hailo_media_buf,cma",
    "/dev/dma_heap/system",
    "/dev/dma_heap/system-uncached",
    "/dev/dma_heap/CMA",
};

struct DmaBuf
{
    int fd = -1;
    size_t size = 0;
    void *map = MAP_FAILED;

    ~DmaBuf()
    {
        if (map != MAP_FAILED)
            munmap(map, size);
        if (fd >= 0)
            close(fd);
    }
};

static int open_dma_heap()
{
    for (const char *path : kHeapCandidates)
    {
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0)
        {
            std::printf("[AB] heap=%s\n", path);
            return fd;
        }
    }
    return -1;
}

static bool dmabuf_alloc(DmaBuf *out, int heap_fd, size_t len)
{
    struct dma_heap_allocation_data data = {};
    data.len = len;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) != 0)
    {
        std::fprintf(stderr, "[AB] DMA_HEAP_IOCTL_ALLOC(len=%zu): %s\n", len, strerror(errno));
        return false;
    }
    out->fd = static_cast<int>(data.fd);
    out->size = len;
    out->map = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, out->fd, 0);
    if (out->map == MAP_FAILED)
    {
        std::fprintf(stderr, "[AB] mmap dmabuf(len=%zu): %s\n", len, strerror(errno));
        close(out->fd);
        out->fd = -1;
        return false;
    }
    return true;
}

/** CPU-side cache flush after pattern writes (dma-buf sync contract). */
static void dmabuf_sync_end(int fd)
{
    struct dma_buf_sync sync = {};
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

/* --------------------------------------------------------- pattern fill -- */

/** Deterministic NV12 pattern; the frame index salts every byte so a stale
 *  NPU binding (returning iteration k-1's pixels) breaks parity. */
static void fill_nv12(uint8_t *y, uint8_t *uv, uint32_t w, uint32_t h, uint32_t frame)
{
    for (uint32_t r = 0; r < h; r++)
        for (uint32_t c = 0; c < w; c++)
            y[(size_t)r * w + c] = static_cast<uint8_t>((r * 7 + c * 13 + frame * 29) & 0xFF);
    const size_t uv_len = (size_t)w * h / 2;
    for (size_t i = 0; i < uv_len; i++)
        uv[i] = static_cast<uint8_t>((i * 11 + frame * 37) & 0xFF);
}

/* ------------------------------------------------------------ one pass -- */

struct RunOut
{
    int rc = -1;
    std::vector<uint64_t> hashes;
};

/** tensor_from_frame -> run -> hash outputs. expect_dma asserts which
 *  tensor_from_frame arm must engage for this frame. */
static RunOut run_frame(HalInferenceSession *sess, const HalModelInfo &mi,
                        const HalFrameBuffer *frame, bool expect_dma)
{
    RunOut r;
    HalTensor in{};
    int rc = HAL_INFERENCE_OPS.tensor_from_frame(frame, &in);
    if (rc != HAL_OK)
    {
        r.rc = rc;
        return r;
    }
    if (expect_dma && (in.data != nullptr || in.dma_fd < 0))
    {
        std::printf("  [AB] fast path NOT engaged (data=%p dma_fd=%d)\n", in.data, in.dma_fd);
        (void)HAL_INFERENCE_OPS.free_tensor(&in);
        r.rc = HAL_ERR_INVALID_STATE;
        return r;
    }
    if (!expect_dma && in.data == nullptr)
    {
        std::printf("  [AB] CPU path produced no staging buffer\n");
        (void)HAL_INFERENCE_OPS.free_tensor(&in);
        r.rc = HAL_ERR_INVALID_STATE;
        return r;
    }

    const int out_count = mi.num_outputs < 4u ? static_cast<int>(mi.num_outputs) : 4;
    HalTensor outs[4]{};
    rc = HAL_INFERENCE_OPS.run(sess, &in, 1, outs, out_count);
    if (rc == HAL_OK)
    {
        for (int i = 0; i < out_count; i++)
        {
            if (outs[i].data && outs[i].byte_size > 0)
                r.hashes.push_back(fnv1a(outs[i].data, outs[i].byte_size));
            else
                r.hashes.push_back(0);
        }
    }
    for (int i = 0; i < out_count; i++)
        (void)HAL_INFERENCE_OPS.free_tensor(&outs[i]);
    (void)HAL_INFERENCE_OPS.free_tensor(&in);
    r.rc = rc;
    return r;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <model.hef> [iters=20] [w h]\n", argv[0]);
        return 2;
    }
    const uint32_t iters = argc >= 3 ? static_cast<uint32_t>(std::atoi(argv[2])) : 20u;

    HalInferenceConfig cfg{};
    std::snprintf(cfg.model_path, sizeof(cfg.model_path), "%s", argv[1]);
    cfg.timeout_ms = 10000;

    HalInferenceSession *sess = HAL_INFERENCE_OPS.create(&cfg);
    if (!sess)
    {
        std::fprintf(stderr, "[AB] create() failed\n");
        return 2;
    }
    HalModelInfo mi{};
    int rc = HAL_INFERENCE_OPS.get_model_info(sess, &mi);
    if (rc != HAL_OK || mi.num_inputs == 0 || mi.num_outputs == 0)
    {
        std::fprintf(stderr, "[AB] get_model_info rc=%d\n", rc);
        (void)HAL_INFERENCE_OPS.destroy(sess);
        return 2;
    }

    /* Model input geometry: NV12 first input, shape[1]=luma H, shape[2]=W
     * (same reconciliation tensor_from_frame_ex uses for its exact-match
     * fast path). CLI w/h override for odd models. */
    uint32_t w = 0, h = 0;
    if (argc >= 5)
    {
        w = static_cast<uint32_t>(std::atoi(argv[3]));
        h = static_cast<uint32_t>(std::atoi(argv[4]));
    }
    else
    {
        const HalModelTensorInfo &in0 = mi.inputs[0];
        if (!in0.is_nv12 || in0.ndim < 3)
        {
            std::fprintf(stderr, "[AB] first input not NV12 (is_nv12=%u ndim=%d) — pass w h\n",
                         (unsigned)in0.is_nv12, in0.ndim);
            (void)HAL_INFERENCE_OPS.destroy(sess);
            return 2;
        }
        w = static_cast<uint32_t>(in0.shape[2]);
        h = static_cast<uint32_t>(in0.shape[1]);
    }
    if (w == 0 || h == 0 || (w & 1) || (h & 1))
    {
        std::fprintf(stderr, "[AB] bad geometry %ux%u (even values required)\n", w, h);
        (void)HAL_INFERENCE_OPS.destroy(sess);
        return 2;
    }
    const uint64_t expect_bytes = (uint64_t)w * h * 3 / 2;
    std::printf("[AB] model=%s input=%ux%u NV12 byte_size=%u (expect %llu) outputs=%u iters=%u\n",
                argv[1], w, h, mi.inputs[0].byte_size, (unsigned long long)expect_bytes,
                mi.num_outputs, iters);

    int heap_fd = open_dma_heap();
    if (heap_fd < 0)
    {
        std::fprintf(stderr, "[AB] no dma_heap available\n");
        (void)HAL_INFERENCE_OPS.destroy(sess);
        return 2;
    }
    DmaBuf ybuf, uvbuf;
    if (!dmabuf_alloc(&ybuf, heap_fd, (size_t)w * h) ||
        !dmabuf_alloc(&uvbuf, heap_fd, (size_t)w * h / 2))
    {
        (void)HAL_INFERENCE_OPS.destroy(sess);
        return 2;
    }
    std::vector<uint8_t> cpu_y((size_t)w * h), cpu_uv((size_t)w * h / 2);

    /* ---- A/B parity loop: fresh salted content every iteration ---- */
    uint32_t pass = 0;
    for (uint32_t i = 0; i < iters; i++)
    {
        fill_nv12(static_cast<uint8_t *>(ybuf.map), static_cast<uint8_t *>(uvbuf.map), w, h, i);
        dmabuf_sync_end(ybuf.fd);
        dmabuf_sync_end(uvbuf.fd);
        std::memcpy(cpu_y.data(), ybuf.map, cpu_y.size());
        std::memcpy(cpu_uv.data(), uvbuf.map, cpu_uv.size());

        HalFrameBuffer fdma{};
        fdma.width = w;
        fdma.height = h;
        fdma.format = HAL_PIX_FMT_NV12;
        fdma.mem_type = HAL_MEM_DMABUF;
        fdma.num_planes = 2;
        /* medialib-style: plane CPU mappings valid alongside the fds, so a
         * contract violation falls through to a working memcpy arm */
        fdma.planes[0] = ybuf.map;
        fdma.planes[1] = uvbuf.map;
        fdma.sizes[0] = (size_t)w * h;
        fdma.sizes[1] = (size_t)w * h / 2;
        fdma.strides[0] = w;
        fdma.strides[1] = w;
        fdma.dma_fds[0] = ybuf.fd;
        fdma.dma_fds[1] = uvbuf.fd;

        const RunOut a = run_frame(sess, mi, &fdma, true);

        HalFrameBuffer fcpu = fdma;
        fcpu.mem_type = HAL_MEM_MALLOC;
        fcpu.planes[0] = cpu_y.data();
        fcpu.planes[1] = cpu_uv.data();
        fcpu.dma_fds[0] = -1;
        fcpu.dma_fds[1] = -1;

        const RunOut b = run_frame(sess, mi, &fcpu, false);

        bool ok = a.rc == HAL_OK && b.rc == HAL_OK && a.hashes.size() == b.hashes.size();
        if (ok)
        {
            for (size_t k = 0; k < a.hashes.size(); k++)
            {
                if (a.hashes[k] != b.hashes[k])
                {
                    ok = false;
                    std::printf("  [AB] iter=%u output[%zu] hash mismatch dma=%016llx cpu=%016llx\n",
                                i, k, (unsigned long long)a.hashes[k], (unsigned long long)b.hashes[k]);
                }
            }
        }
        else if (a.rc != HAL_OK || b.rc != HAL_OK)
        {
            std::printf("  [AB] iter=%u run rc dma=%d(%s) cpu=%d(%s)\n", i,
                        a.rc, hal_error_to_string((HalErrorCode)a.rc),
                        b.rc, hal_error_to_string((HalErrorCode)b.rc));
        }
        if (ok)
            pass++;
    }

    /* ---- mismatch contract: wrong declared geometry must fail cleanly at
     * run() (bind-time byte_size == frame_size), never garbage results ---- */
    HalFrameBuffer fbad{};
    fbad.width = w + 2;
    fbad.height = h;
    fbad.format = HAL_PIX_FMT_NV12;
    fbad.mem_type = HAL_MEM_DMABUF;
    fbad.num_planes = 2;
    fbad.planes[0] = ybuf.map;
    fbad.planes[1] = uvbuf.map;
    fbad.sizes[0] = (size_t)(w + 2) * h;
    fbad.sizes[1] = (size_t)(w + 2) * h / 2;
    fbad.strides[0] = w + 2;
    fbad.strides[1] = w + 2;
    fbad.dma_fds[0] = ybuf.fd;
    fbad.dma_fds[1] = uvbuf.fd;
    const RunOut bad = run_frame(sess, mi, &fbad, true);
    const bool mismatch_ok = bad.rc == HAL_ERR_INVALID_SIZE;
    std::printf("[AB] mismatch-contract: rc=%d (%s) verdict=%s\n", bad.rc,
                hal_error_to_string((HalErrorCode)bad.rc), mismatch_ok ? "OK" : "FAIL");

    const bool verdict = pass == iters && mismatch_ok;
    std::printf("[AB] verdict: %s parity=%u/%u fastpath=ENGAGED mismatch=%s\n",
                verdict ? "PASS" : "FAIL", pass, iters, mismatch_ok ? "OK" : "FAIL");

    (void)HAL_INFERENCE_OPS.destroy(sess);
    close(heap_fd);
    return verdict ? 0 : 1;
}
