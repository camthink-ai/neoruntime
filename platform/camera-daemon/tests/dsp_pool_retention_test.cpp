/**
 * @file dsp_pool_retention_test.cpp
 * @brief P1-9 — DspService pool-retention cache (the 43ms-tail fix).
 *
 * Convicted root cause of the resize bimodality: the SDK accel router
 * creates a fresh client per call, so each call allocs per-call temp
 * pools and releases them right after — and the HAL pool cache is weak
 * (hailo15_media_impl "pool is freed when last buffer releases it"),
 * so every cycle destroys and recreates the vendor 32-buffer dma chunk.
 * The chunk-recreate collides with the prior call's background
 * chunk-free: p99 80ms, max 104ms.
 *
 * Fix under test: released non-imported pool buffers are PARKED by
 * geometry {w,h,fmt} for cfg.pool_retention_ms; alloc_buffers serves
 * from the park first (zero HAL rounds, keeps the vendor chunk alive).
 * Footprint is chunk-accurate (one parked buffer pins the whole vendor
 * chunk): first park of a geometry adds kPoolChunkBuffers*bytes, and
 * over-cap pressure evicts whole geometries oldest-first. Imports are
 * never parked (their memory is the client's). stop() flushes.
 *
 * Host-only harness, same fake-op style as dsp_lane_split_test: fake
 * HalFrameBufferOps hands out heap NV12 buffers with DISTINGUISHABLE
 * dma_fds (fd equality is how a reuse is proven), counters track every
 * HAL round.
 */

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/mman.h> /* memfd_create */
#include <unistd.h>   /* ftruncate, close */

#include "dsp_service.h"

/* ---------------- fake ops + counters ---------------- */

static int g_request_count = 0; /* HAL pool rounds                */
static int g_release_count = 0; /* HAL buffer frees               */
static int g_next_fd = 100;     /* distinctive dma_fds per buffer */

static int fake_dsp_init(const HalDspConfig*, void** ctx) {
    *ctx = (void*)0x1;
    return 0;
}
static int fake_dsp_deinit(void*) { return 0; }

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
    for (uint32_t p = 0; p < fb->num_planes; ++p) fb->dma_fds[p] = g_next_fd++;
    fb->strides[0] = req->width;
    fb->sizes[0] = req->width * req->height;
    if (fb->num_planes == 2) {
        fb->strides[1] = req->width;
        fb->sizes[1] = req->width * req->height / 2;
    }
    g_request_count++;
    *out = fb;
    return 0;
}
static int fake_release_fb(HalFrameBuffer* fb) {
    g_release_count++;
    delete fb;
    return 0;
}

/* chunk math the fake implies: per-buffer NV12 bytes = w*h*3/2, so a
 * chunk of 32 buffers is 48*w*h bytes. 32x32 → 49152, 40x40 → 76800. */

static void reset_globals() {
    g_request_count = 0;
    g_release_count = 0;
    g_next_fd = 100;
}

/* DspService is non-copyable/non-movable — construct it in place in
 * each case against these shared file-scope ops tables. */
static HalDspOps g_dsp_ops = [] {
    HalDspOps ops = {};
    ops.init = fake_dsp_init;
    ops.deinit = fake_dsp_deinit;
    return ops;
}();
static HalFrameBufferOps g_fb_ops = [] {
    HalFrameBufferOps ops = {};
    ops.request_frame_buffer = fake_request_fb;
    ops.release_frame_buffer = fake_release_fb;
    return ops;
}();

/* alloc one NV12 buffer; asserts success and returns the result. */
static DspService::AllocResult alloc_one(DspService& dsp, int client_fd,
                                         uint32_t w, uint32_t h) {
    auto r = dsp.alloc_buffers(client_fd, w, h, HAL_PIX_FMT_NV12, 1);
    assert(r.rc == DSP_SVC_OK);
    assert(r.ids.size() == 1);
    assert(r.fds.size() == 2); /* NV12 = 2 planes */
    return r;
}

/* ---------------- cases ---------------- */

static void case_reuse_hit() {
    /* Release parks; the next same-geometry alloc is served from the
     * park with ZERO new HAL rounds — no alloc, no release, fds handed
     * back verbatim under a fresh registry id. */
    reset_globals();
    DspServiceConfig cfg; /* defaults: ms=3000, cap=192MiB */
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    auto r1 = alloc_one(dsp, 7, 32, 32);
    assert(g_request_count == 1);
    assert(dsp.release_buffer(7, r1.ids[0]) == DSP_SVC_OK);
    assert(dsp.stats().retention_parked == 1);

    auto r2 = alloc_one(dsp, 7, 32, 32);
    assert(g_request_count == 1); /* no new HAL pool round        */
    assert(g_release_count == 0); /* nothing freed                */
    const auto& st = dsp.stats();
    assert(st.retention_reuses == 1);
    assert(st.retention_parked == 0);
    assert(r2.ids[0] != r1.ids[0]); /* fresh registry id           */
    assert(r2.fds[0] == r1.fds[0]); /* same underlying dma buffer  */
    assert(r2.fds[1] == r1.fds[1]);

    dsp.stop();
    dsp.release_client_buffers(7);
    std::printf("  case_reuse_hit ok\n");
}

static void case_lazy_expiry() {
    /* An expired park is dropped by take_parked itself (lazy, on the
     * next same-geometry alloc): HAL release + fresh request. */
    reset_globals();
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 50;
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    auto r1 = alloc_one(dsp, 7, 32, 32);
    assert(dsp.release_buffer(7, r1.ids[0]) == DSP_SVC_OK);
    usleep(120 * 1000); /* past the 50ms grace */

    auto r2 = alloc_one(dsp, 7, 32, 32);
    assert(g_request_count == 2); /* expired park → fresh HAL alloc */
    assert(g_release_count == 1); /* expired park dropped to HAL    */
    const auto& st = dsp.stats();
    assert(st.retention_reuses == 0);
    assert(st.retention_releases == 1);
    assert(st.retention_parked == 0);
    assert(r2.fds[0] != r1.fds[0]); /* a different buffer           */

    dsp.stop();
    dsp.release_client_buffers(7);
    std::printf("  case_lazy_expiry ok\n");
}

static void case_sweep_expiry() {
    /* Without any alloc, the worker's 1s idle wake sweeps expired
     * parks to HAL — retention cannot leak memory past its grace. */
    reset_globals();
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 50;
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    auto r1 = alloc_one(dsp, 7, 32, 32);
    assert(dsp.release_buffer(7, r1.ids[0]) == DSP_SVC_OK);
    assert(dsp.stats().retention_parked == 1);

    usleep(1500 * 1000); /* > 1s worker tick + 50ms grace */
    const auto& st = dsp.stats();
    assert(st.retention_releases == 1);
    assert(st.retention_parked == 0);
    assert(g_release_count == 1);

    dsp.stop();
    dsp.release_client_buffers(7);
    std::printf("  case_sweep_expiry ok\n");
}

static void case_imports_never_parked() {
    /* Imported (USERPTR) buffers hold the CLIENT's memory — parking
     * one would be a lifetime bug, not a reuse. Release frees it. */
    reset_globals();
    DspServiceConfig cfg;
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    const uint32_t strides[2] = {96, 96};
    const uint32_t sizes[2] = {96 * 64, 96 * 64 / 2};
    int fd = memfd_create("retention-import", 0);
    assert(fd >= 0);
    assert(ftruncate(fd, (off_t)(sizes[0] + sizes[1])) == 0);
    const int fds[2] = {fd, fd}; /* two-plane: real 2-entry array */
    auto r = dsp.import_buffer(7, 96, 64, HAL_PIX_FMT_NV12, 2, strides,
                               sizes, fds);
    close(fd);
    assert(r.rc == DSP_SVC_OK);

    assert(dsp.release_buffer(7, r.id) == DSP_SVC_OK);
    const auto& st = dsp.stats();
    assert(st.retention_parked == 0);
    assert(st.retention_reuses == 0);
    assert(st.retention_releases == 0);
    assert(g_release_count == 0); /* import frees don't touch fb_ops */

    dsp.stop();
    dsp.release_client_buffers(7);
    std::printf("  case_imports_never_parked ok\n");
}

static void case_cap_evicts_oldest_geometry() {
    /* Footprint is chunk-accurate. 32x32 chunk = 49152 B, 40x40 chunk
     * = 76800 B; cap = 76800 → parking G2 must evict G1 WHOLLY. */
    reset_globals();
    DspServiceConfig cfg;
    cfg.pool_retention_max_bytes = 76800;
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    auto g1a = alloc_one(dsp, 7, 32, 32); /* chunk 49152 */
    assert(dsp.release_buffer(7, g1a.ids[0]) == DSP_SVC_OK);
    assert(dsp.stats().retention_parked == 1);

    auto g2a = alloc_one(dsp, 7, 40, 40); /* chunk 76800 */
    assert(dsp.release_buffer(7, g2a.ids[0]) == DSP_SVC_OK);
    {
        const auto& st = dsp.stats();
        assert(st.retention_parked == 1); /* only G2 survives        */
        assert(st.retention_releases == 1); /* G1 evicted            */
    }
    assert(g_release_count == 1); /* G1's buffer handed to HAL       */

    /* G1 is gone from the park → fresh HAL round; G2 survives → reuse. */
    auto g1b = alloc_one(dsp, 7, 32, 32);
    assert(g_request_count == 3); /* 2 originals + G1 re-alloc       */
    auto g2b = alloc_one(dsp, 7, 40, 40);
    assert(g_request_count == 3); /* served from the park            */
    assert(g2b.fds[0] == g2a.fds[0]);
    assert(dsp.stats().retention_reuses == 1);

    dsp.stop();
    dsp.release_client_buffers(7);
    std::printf("  case_cap_evicts_oldest_geometry ok\n");
}

static void case_disabled_by_config() {
    /* Either knob at 0 disables retention — the rollback path: every
     * release frees immediately, every alloc is a fresh HAL round. */
    reset_globals();
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 0;
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    auto r1 = alloc_one(dsp, 7, 32, 32);
    assert(dsp.release_buffer(7, r1.ids[0]) == DSP_SVC_OK);
    assert(g_release_count == 1); /* immediate free, no park */
    auto r2 = alloc_one(dsp, 7, 32, 32);
    assert(g_request_count == 2);
    const auto& st = dsp.stats();
    assert(st.retention_parked == 0);
    assert(st.retention_reuses == 0);
    assert(st.retention_releases == 0);
    assert(r2.fds[0] != r1.fds[0]);

    dsp.stop();
    dsp.release_client_buffers(7);
    std::printf("  case_disabled_by_config ok\n");
}

static void case_stop_flushes_parks() {
    /* stop() must not strand parked vendor chunks: everything parked
     * goes back to HAL before the contexts deinit. */
    reset_globals();
    DspServiceConfig cfg;
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    auto r1 = alloc_one(dsp, 7, 32, 32);
    assert(dsp.release_buffer(7, r1.ids[0]) == DSP_SVC_OK);
    assert(dsp.stats().retention_parked == 1);

    dsp.stop();
    const auto& st = dsp.stats();
    assert(st.retention_parked == 0);
    assert(st.retention_releases == 1);
    assert(g_release_count == 1);

    dsp.release_client_buffers(7);
    std::printf("  case_stop_flushes_parks ok\n");
}

int main() {
    case_reuse_hit();
    case_lazy_expiry();
    case_sweep_expiry();
    case_imports_never_parked();
    case_cap_evicts_oldest_geometry();
    case_disabled_by_config();
    case_stop_flushes_parks();
    std::printf("dsp_pool_retention_test: all assertions passed\n");
    return 0;
}
