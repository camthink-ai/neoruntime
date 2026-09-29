/**
 * @file dsp_lane_split_test.cpp
 * @brief P1-9 — DspService per-lane vendor priority split.
 *
 * Root cause being pinned here: app jobs and stream-side DSP work (dpm_worker
 * resizes, encoder) all sit at vendor device priority 0, so an app resize
 * FIFOs behind ~30-40ms stream tasks on the 30fps grid — the 8ms/43ms
 * bimodality in ab_resize_nv12_default. The vendor reorders QUEUED work by
 * per-device priority ("higher executes first", dsp_set_priority), and the
 * HAL plumbs HalDspConfig.device_priority — so the fix is per-lane contexts:
 *
 *   NORMAL lane     → cfg.device_priority (default 1, ahead of stream work)
 *   BACKGROUND lane → 0 (deliberately ties with stream work; a bulk lane
 *                     must not preempt the encoder)
 *
 * In-service ordering (NORMAL deque drains before BACKGROUND) already
 * exists; this test pins the CONTEXT routing and the init contract.
 *
 * Host-only harness: fake HalDspOps records the device_priority of every
 * init and the ctx every resize ran on; fake HalFrameBufferOps hands out
 * plain-heap NV12 pool buffers (imported memfd plays the source).
 */

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/mman.h> /* memfd_create */
#include <unistd.h>   /* ftruncate, close */

#include "dsp_service.h"

/* ---------------- fake HalDspOps ---------------- */

/* Distinguishable ctx identities handed out by init. */
static void* const kCtxA = (void*)0x10;
static void* const kCtxB = (void*)0x20;

static std::vector<int> g_init_prios;      /* device_priority per init call */
static int g_deinit_count = 0;
static int g_init_fail_at = -1;            /* fail the Nth init (0-based)   */

static int fake_dsp_init(const HalDspConfig* cfg, void** ctx) {
    const int idx = static_cast<int>(g_init_prios.size());
    g_init_prios.push_back(cfg->device_priority);
    if (idx == g_init_fail_at) return -1;
    *ctx = (idx == 0) ? kCtxA : kCtxB;
    return 0;
}
static int fake_dsp_deinit(void*) {
    g_deinit_count++;
    return 0;
}

static std::vector<void*> g_resize_ctx_log; /* ctx per executed resize */
static int fake_dsp_resize(void* ctx, const HalDspResizeParams*) {
    g_resize_ctx_log.push_back(ctx);
    return 0;
}

/* ---------------- fake HalFrameBufferOps ---------------- */

static int fake_request_fb(const HalFrameBufferRequest* req,
                           HalFrameBuffer** out) {
    auto* fb = new HalFrameBuffer();
    std::memset(fb, 0, sizeof(*fb));
    fb->width = req->width;
    fb->height = req->height;
    fb->format = req->format;
    fb->mem_type = HAL_MEM_DMABUF;
    fb->num_planes = (req->format == HAL_PIX_FMT_NV12) ? 2 : 1;
    for (uint32_t p = 0; p < HAL_MAX_PLANES; ++p) fb->dma_fds[p] = -1;
    fb->strides[0] = req->width;
    fb->sizes[0] = req->width * req->height;
    if (fb->num_planes == 2) {
        fb->strides[1] = req->width;
        fb->sizes[1] = req->width * req->height / 2;
    }
    *out = fb;
    return 0;
}
static int fake_release_fb(HalFrameBuffer* fb) {
    delete fb;
    return 0;
}

/* ---------------- helpers ---------------- */

/* One 64x64 NV12 source imported from a memfd (the USERPTR route). */
static uint64_t import_src(DspService& dsp, int client_fd) {
    const uint32_t strides[2] = {64, 64};
    const uint32_t sizes[2] = {64 * 64, 64 * 64 / 2};
    int fd = memfd_create("lane-split-src", 0);
    assert(fd >= 0);
    assert(ftruncate(fd, (off_t)(sizes[0] + sizes[1])) == 0);
    /* NV12 is two-plane: fds[] must be a real 2-entry array — the service
     * dups each plane's fd, so passing &fd would read fds[1] as stack
     * garbage. Both planes may share one memfd: the mmap route only needs
     * mappable fds, nothing ever touches the pixels. */
    const int fds[2] = {fd, fd};
    auto r = dsp.import_buffer(client_fd, 64, 64, HAL_PIX_FMT_NV12, 2,
                               strides, sizes, fds);
    close(fd); /* registry dup'd its own copy */
    assert(r.rc == DSP_SVC_OK);
    assert(r.id != 0);
    return r.id;
}

/* One 32x32 NV12 pool dst owned by client_fd. */
static uint64_t alloc_dst(DspService& dsp, int client_fd) {
    auto r = dsp.alloc_buffers(client_fd, 32, 32, HAL_PIX_FMT_NV12, 1);
    assert(r.rc == DSP_SVC_OK);
    assert(r.ids.size() == 1);
    return r.ids[0];
}

/* Submit one RESIZE job at `prio`; asserts it executed and returns which
 * ctx the HAL op ran on. */
static void* run_resize(DspService& dsp, uint64_t src, uint64_t dst,
                        DspPriority prio) {
    DspJobDesc d;
    d.op = HAL_DSP_OP_RESIZE;
    d.src_id = src;
    d.dst_ids = {dst};
    d.priority = prio;
    const size_t before = g_resize_ctx_log.size();
    DspJobResult res = dsp.submit_job(d);
    assert(res.rc == DSP_SVC_OK);
    assert(g_resize_ctx_log.size() == before + 1);
    return g_resize_ctx_log.back();
}

static void reset_globals() {
    g_init_prios.clear();
    g_deinit_count = 0;
    g_init_fail_at = -1;
    g_resize_ctx_log.clear();
}

/* ---------------- cases ---------------- */

static void case_default_split() {
    reset_globals();
    HalDspOps dsp_ops = {};
    dsp_ops.init = fake_dsp_init;
    dsp_ops.deinit = fake_dsp_deinit;
    dsp_ops.resize = fake_dsp_resize;
    HalFrameBufferOps fb_ops = {};
    fb_ops.request_frame_buffer = fake_request_fb;
    fb_ops.release_frame_buffer = fake_release_fb;

    DspServiceConfig cfg; /* defaults under test */
    DspService dsp(&dsp_ops, &fb_ops, cfg);
    assert(dsp.start());

    /* Init contract: two lanes — NORMAL at cfg.device_priority (default
     * 1, above stream work), BACKGROUND at 0 (ties with the encoder). */
    assert(g_init_prios.size() == 2);
    assert(g_init_prios[0] == cfg.device_priority);
    assert(cfg.device_priority == 1); /* the P1-9 default itself */
    assert(g_init_prios[1] == 0);

    uint64_t src = import_src(dsp, 7);
    uint64_t dst = alloc_dst(dsp, 7);

    /* Routing: each lane's jobs execute on its own HAL context. */
    assert(run_resize(dsp, src, dst, DspPriority::Normal) == kCtxA);
    assert(run_resize(dsp, src, dst, DspPriority::Background) == kCtxB);
    assert(run_resize(dsp, src, dst, DspPriority::Normal) == kCtxA);

    dsp.stop();
    assert(g_deinit_count == 2); /* both lanes torn down */

    dsp.release_client_buffers(7);
    reset_globals();
    std::printf("  case_default_split ok\n");
}

static void case_config_override_zero() {
    /* device_priority=0 restores the pre-P1-9 flat behavior (both lanes
     * at vendor default) — the rollback knob. */
    reset_globals();
    HalDspOps dsp_ops = {};
    dsp_ops.init = fake_dsp_init;
    dsp_ops.deinit = fake_dsp_deinit;
    dsp_ops.resize = fake_dsp_resize;
    HalFrameBufferOps fb_ops = {};
    fb_ops.request_frame_buffer = fake_request_fb;
    fb_ops.release_frame_buffer = fake_release_fb;

    DspServiceConfig cfg;
    cfg.device_priority = 0;
    DspService dsp(&dsp_ops, &fb_ops, cfg);
    assert(dsp.start());
    assert(g_init_prios.size() == 2);
    assert(g_init_prios[0] == 0);
    assert(g_init_prios[1] == 0);
    dsp.stop();
    reset_globals();
    std::printf("  case_config_override_zero ok\n");
}

static void case_background_init_failure_fallback() {
    /* If the BACKGROUND-lane context cannot be created, the service must
     * still start; BACKGROUND jobs then ride the NORMAL context (a
     * scheduling-semantics degradation, not a fatal fault). */
    reset_globals();
    g_init_fail_at = 1; /* second init fails */
    HalDspOps dsp_ops = {};
    dsp_ops.init = fake_dsp_init;
    dsp_ops.deinit = fake_dsp_deinit;
    dsp_ops.resize = fake_dsp_resize;
    HalFrameBufferOps fb_ops = {};
    fb_ops.request_frame_buffer = fake_request_fb;
    fb_ops.release_frame_buffer = fake_release_fb;

    DspService dsp(&dsp_ops, &fb_ops);
    assert(dsp.start());

    uint64_t src = import_src(dsp, 7);
    uint64_t dst = alloc_dst(dsp, 7);
    assert(run_resize(dsp, src, dst, DspPriority::Normal) == kCtxA);
    assert(run_resize(dsp, src, dst, DspPriority::Background) == kCtxA);

    dsp.stop();
    dsp.release_client_buffers(7);
    reset_globals();
    std::printf("  case_background_init_failure_fallback ok\n");
}

static void case_normal_init_failure_is_fatal() {
    /* No NORMAL lane → no service (same contract as the old single-ctx
     * start failure). */
    reset_globals();
    g_init_fail_at = 0;
    HalDspOps dsp_ops = {};
    dsp_ops.init = fake_dsp_init;
    dsp_ops.deinit = fake_dsp_deinit;
    dsp_ops.resize = fake_dsp_resize;
    HalFrameBufferOps fb_ops = {};
    fb_ops.request_frame_buffer = fake_request_fb;
    fb_ops.release_frame_buffer = fake_release_fb;

    DspService dsp(&dsp_ops, &fb_ops);
    assert(!dsp.start());
    reset_globals();
    std::printf("  case_normal_init_failure_is_fatal ok\n");
}

int main() {
    case_default_split();
    case_config_override_zero();
    case_background_init_failure_fallback();
    case_normal_init_failure_is_fatal();
    std::printf("dsp_lane_split_test: all assertions passed\n");
    return 0;
}
