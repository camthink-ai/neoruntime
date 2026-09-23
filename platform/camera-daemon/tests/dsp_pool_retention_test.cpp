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
static int g_last_pool_max = 0; /* pool_max_buffers of last HAL request */

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
    g_last_pool_max = static_cast<int>(req->pool_max_buffers);
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
    g_last_pool_max = 0;
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
    /* Footprint is chunk-accurate and the cap still bounds the TOTAL.
     * Budget-half sizing makes any TWO geometries coexist by design
     * (that is the src+dst fix) — eviction needs a third: cap 57600
     * holds G1(32x32 → 18 bufs × 1536B = 27648) + G2(40x40 → 12 × 2400
     * = 28800) = 56448, but parking G3 (48x48 → 8 × 3456 = 27648) must
     * evict G1 WHOLLY, oldest-first. */
    reset_globals();
    DspServiceConfig cfg;
    cfg.pool_retention_max_bytes = 57600;
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    auto g1a = alloc_one(dsp, 7, 32, 32); /* chunk 27648 */
    assert(dsp.release_buffer(7, g1a.ids[0]) == DSP_SVC_OK);
    assert(dsp.stats().retention_parked == 1);

    auto g2a = alloc_one(dsp, 7, 40, 40); /* chunk 28800 */
    assert(dsp.release_buffer(7, g2a.ids[0]) == DSP_SVC_OK);
    assert(dsp.stats().retention_parked == 2); /* pair coexists — the
                                                * src+dst guarantee    */

    auto g3a = alloc_one(dsp, 7, 48, 48); /* chunk 27648 */
    assert(dsp.release_buffer(7, g3a.ids[0]) == DSP_SVC_OK);
    {
        const auto& st = dsp.stats();
        assert(st.retention_parked == 2); /* G2+G3 survive            */
        assert(st.retention_releases == 1); /* G1 evicted (oldest)    */
    }
    assert(g_release_count == 1); /* G1's buffer handed to HAL        */

    /* G1 is gone from the park → fresh HAL round; G2 survives → reuse. */
    auto g1b = alloc_one(dsp, 7, 32, 32);
    assert(g_request_count == 4); /* 3 originals + G1 re-alloc        */
    auto g2b = alloc_one(dsp, 7, 40, 40);
    assert(g_request_count == 4); /* served from the park             */
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

/* ---------------- retention-sized chunks (the 4K array-path fix) -----
 * The regression: a geometry whose full 32-buffer chunk exceeds
 * pool_retention_max_bytes (4K NV12 ≈ 372MiB vs 192MiB) is never parked,
 * so per-call alloc/release destroys and rebuilds the vendor chunk every
 * round. Fix: alloc sizes the pool chunk to HALF the parking budget
 * (retention_pool_chunk_n) — half, not whole, because one resize parks
 * src AND dst and whole-budget chunks made the pair evict each other
 * every round (the 148ms plateau). Fake math: per-buffer NV12 bytes =
 * w*h*3/2 exactly (no alignment), so 3840x2160 → 12,441,600 B/buffer;
 * 96MiB/12.44MB → 8 buffers → chunk 94.9MiB ≤ 192MiB → parks. */

static void case_retention_sizes_big_pool() {
    /* 4K-class geometry: the HAL request asks for a budget-half chunk
     * (8, not 32) and a release→alloc cycle is served from the park. */
    reset_globals();
    DspServiceConfig cfg; /* defaults: ms=3000, cap=192MiB */
    cfg.pool_retention_max_bytes = 192ULL << 20; /* pin: cases must not
                                                  * track default drift */
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    auto r1 = alloc_one(dsp, 7, 3840, 2160);
    assert(g_request_count == 1);
    assert(g_last_pool_max == 8);
    assert(dsp.release_buffer(7, r1.ids[0]) == DSP_SVC_OK);
    assert(dsp.stats().retention_parked == 1);

    auto r2 = alloc_one(dsp, 7, 3840, 2160);
    assert(g_request_count == 1); /* reuse, no HAL pool round           */
    assert(g_release_count == 0);
    assert(dsp.stats().retention_reuses == 1);
    assert(r2.fds[0] == r1.fds[0]);

    dsp.stop();
    dsp.release_client_buffers(7);
    std::printf("  case_retention_sizes_big_pool ok\n");
}

static void case_src_dst_geometries_coexist() {
    /* The two-geometry lesson, as a regression gate: one resize call
     * parks src AND dst. Budget-HALF chunks must let a giant src and a
     * regular dst park together without evicting each other — 4K(8x
     * 12.44MB = 94.9MiB) + 1080p(32x 3.11MB = 94.9MiB) = 189.9MiB ≤
     * 192MiB. Whole-budget chunks (16+32 buffers) thrashed here: each
     * dst park evicted src, and src re-acquired from HAL every round. */
    reset_globals();
    DspServiceConfig cfg;
    cfg.pool_retention_max_bytes = 192ULL << 20;
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    auto s1 = alloc_one(dsp, 7, 3840, 2160); /* src geometry  */
    auto d1 = alloc_one(dsp, 7, 1920, 1080); /* dst geometry  */
    assert(g_last_pool_max == 32); /* sub-4K: fd ceiling, retention ON */
    assert(dsp.release_buffer(7, s1.ids[0]) == DSP_SVC_OK);
    assert(dsp.stats().retention_parked == 1);
    assert(dsp.release_buffer(7, d1.ids[0]) == DSP_SVC_OK);
    assert(dsp.stats().retention_parked == 2); /* BOTH live — no evict */
    assert(g_release_count == 0);              /* nothing handed back  */

    /* Next round serves both geometries from the park: zero HAL rounds. */
    auto s2 = alloc_one(dsp, 7, 3840, 2160);
    auto d2 = alloc_one(dsp, 7, 1920, 1080);
    assert(g_request_count == 2); /* only the two originals             */
    assert(g_release_count == 0);
    assert(dsp.stats().retention_reuses == 2);
    assert(s2.fds[0] == s1.fds[0]);
    assert(d2.fds[0] == d1.fds[0]);

    dsp.stop();
    dsp.release_client_buffers(7);
    std::printf("  case_src_dst_geometries_coexist ok\n");
}

static void case_count_raises_chunk_over_cap() {
    /* count can raise the chunk past the parking budget — the request
     * must still succeed (a count-capped alloc must not fail on a
     * shrunken pool), it just doesn't park: the raised chunk exceeds the
     * cap and is refused, exactly the pre-fix churn behavior for it. */
    reset_globals();
    DspServiceConfig cfg;
    cfg.pool_retention_max_bytes = 192ULL << 20;
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    auto r = dsp.alloc_buffers(7, 3840, 2160, HAL_PIX_FMT_NV12, 17);
    assert(r.rc == DSP_SVC_OK);
    assert(r.ids.size() == 17);
    assert(g_last_pool_max == 17); /* max(retention_n=8, count=17)      */
    for (uint64_t id : r.ids)
        assert(dsp.release_buffer(7, id) == DSP_SVC_OK);
    assert(dsp.stats().retention_parked == 0); /* 17-buf chunk > cap    */
    assert(g_release_count == 17);             /* all freed to HAL      */

    dsp.stop();
    dsp.release_client_buffers(7);
    std::printf("  case_count_raises_chunk_over_cap ok\n");
}

static void case_count_fitting_parks_and_evicts() {
    /* The fitting twin of the refusal case: count=15 at 4K raises the
     * chunk to 15 (186,624,000 B = 177.9 MiB) which still fits a 180MiB
     * cap → it PARKS. And the park must be accounted at the real
     * 15-buffer chunk: a following small-geometry park (1024×512 → 32 ×
     * 786,432 = 25,165,824) pushes the total to 211,789,824 > 180MiB
     * and evicts the WHOLE 4K geometry. Retention-sized accounting
     * (7 × 12,441,600 = 87M, total 112M ≤ cap) would keep it parked —
     * so this also pins the park-side charge at pool_key.max_buffers. */
    reset_globals();
    DspServiceConfig cfg;
    cfg.pool_retention_max_bytes = 180ULL << 20;
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    auto r = dsp.alloc_buffers(7, 3840, 2160, HAL_PIX_FMT_NV12, 15);
    assert(r.rc == DSP_SVC_OK);
    assert(r.ids.size() == 15);
    assert(g_last_pool_max == 15); /* max(retention_n=7, count=15) */
    for (uint64_t id : r.ids)
        assert(dsp.release_buffer(7, id) == DSP_SVC_OK);
    assert(dsp.stats().retention_parked == 15); /* fits, parks */
    assert(g_release_count == 0);

    auto small = alloc_one(dsp, 7, 1024, 512);
    assert(g_last_pool_max == 32); /* sub-4K keeps the fd ceiling */
    assert(dsp.release_buffer(7, small.ids[0]) == DSP_SVC_OK);
    {
        const auto& st = dsp.stats();
        assert(st.retention_parked == 1); /* only the small geometry  */
        assert(st.retention_releases == 15); /* the 4K chunk evicted  */
    }
    assert(g_release_count == 15); /* the 15-buffer chunk to HAL      */

    dsp.stop();
    dsp.release_client_buffers(7);
    std::printf("  case_count_fitting_parks_and_evicts ok\n");
}

static void case_reuse_parks_source_chunk() {
    /* Provenance: a reused buffer must park at its SOURCE pool's chunk,
     * not the reusing call's own sizing. Park a count-raised 15-buffer
     * 4K chunk (186.6M under the 192MiB cap), then a count=20 call
     * reuses all 15 (its chunk_n is 20 — LARGER here) and allocates 5
     * fresh. On release the 15 reuses must re-park at 186.6M and only
     * the 5 fresh (20 × 12,441,600 = 248.8M > cap) may be refused. The
     * pre-provenance code stamped every entry with THIS call's chunk, so
     * all 15 reuses were refused too — parked would drop to 0. */
    reset_globals();
    DspServiceConfig cfg;
    cfg.pool_retention_max_bytes = 192ULL << 20;
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    auto r1 = dsp.alloc_buffers(7, 3840, 2160, HAL_PIX_FMT_NV12, 15);
    assert(r1.rc == DSP_SVC_OK);
    for (uint64_t id : r1.ids)
        assert(dsp.release_buffer(7, id) == DSP_SVC_OK);
    assert(dsp.stats().retention_parked == 15);

    auto r2 = dsp.alloc_buffers(7, 3840, 2160, HAL_PIX_FMT_NV12, 20);
    assert(r2.rc == DSP_SVC_OK);
    assert(g_last_pool_max == 20); /* max(retention_n=8, count=20) */
    assert(g_request_count == 20); /* 15 + 5 fresh; 15 were reuses   */
    for (uint64_t id : r2.ids)
        assert(dsp.release_buffer(7, id) == DSP_SVC_OK);
    {
        const auto& st = dsp.stats();
        assert(st.retention_parked == 15); /* reuses re-parked       */
        assert(st.retention_reuses == 15);
    }
    assert(g_release_count == 5); /* only the oversize fresh refused */

    /* Full drain and re-park: the 15 parked serve a count=15 alloc with
     * ZERO fresh HAL rounds, the residency debits exactly its credited
     * chunk, and stop() flushes 15 + the 5 earlier refusals. */
    auto r3 = dsp.alloc_buffers(7, 3840, 2160, HAL_PIX_FMT_NV12, 15);
    assert(r3.rc == DSP_SVC_OK);
    assert(g_request_count == 20); /* all 15 from the park           */
    assert(dsp.stats().retention_reuses == 30);
    for (uint64_t id : r3.ids)
        assert(dsp.release_buffer(7, id) == DSP_SVC_OK);
    assert(dsp.stats().retention_parked == 15);

    dsp.stop();
    dsp.release_client_buffers(7);
    assert(g_release_count == 20); /* 5 refusals + 15 flushed        */
    std::printf("  case_reuse_parks_source_chunk ok\n");
}

static void case_mixed_chunks_ledger_exact() {
    /* The ledger must stay exact when ONE geometry's deque mixes chunk
     * sizes (retention-sized 8-chunk re-parked beside a count-raised
     * 15-chunk) — credit once at first park, debit that same remembered
     * chunk on every removal path. Old code debited the front/back
     * buffer's chunk, so a mixed drain subtracted 186.6M for a 99.5M
     * credit and the clamp silently ate the small geometry's 25.2M —
     * the final park below then fails to evict what it must. */
    reset_globals();
    DspServiceConfig cfg;
    cfg.pool_retention_max_bytes = 192ULL << 20;
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    /* (a) an 8-buffer retention-sized chunk parks: 8 × 12,441,600. */
    auto a = dsp.alloc_buffers(7, 3840, 2160, HAL_PIX_FMT_NV12, 8);
    assert(a.rc == DSP_SVC_OK && g_last_pool_max == 8);
    for (uint64_t id : a.ids)
        assert(dsp.release_buffer(7, id) == DSP_SVC_OK);

    /* (b) count=15 drains the 8 parked (provenance 99.5M each) and
     * adds 7 fresh from a 15-pool (chunk 186.6M ≤ cap, parks too). */
    auto b = dsp.alloc_buffers(7, 3840, 2160, HAL_PIX_FMT_NV12, 15);
    assert(b.rc == DSP_SVC_OK && g_last_pool_max == 15);
    for (uint64_t id : b.ids)
        assert(dsp.release_buffer(7, id) == DSP_SVC_OK);
    {
        const auto& st = dsp.stats();
        assert(st.retention_parked == 15); /* 8@99.5M + 7@186.6M      */
        assert(st.retention_releases == 0); /* no bogus eviction      */
    }
    assert(g_release_count == 0);

    /* (c) a small geometry parks beside it: 99.5M + 25.2M fits. */
    auto small = alloc_one(dsp, 7, 1024, 512);
    assert(dsp.release_buffer(7, small.ids[0]) == DSP_SVC_OK);
    assert(dsp.stats().retention_parked == 16);
    assert(g_request_count == 16); /* 8 + 7 + 1, rest were reuses     */

    /* (d) drain the mixed deque: the residency must debit its credited
     * 99.5M, leaving the small geometry's 25.2M in the footprint. */
    auto d = dsp.alloc_buffers(7, 3840, 2160, HAL_PIX_FMT_NV12, 15);
    assert(d.rc == DSP_SVC_OK);
    assert(g_request_count == 16); /* zero fresh — 15 reuses         */
    for (uint64_t id : d.ids)
        assert(dsp.release_buffer(7, id) == DSP_SVC_OK);
    assert(dsp.stats().retention_parked == 16);

    /* (e) a 2560×1440 chunk (18 × 5,529,600 = 99.5M) pushes the true
     * footprint to 124.7M + 99.5M > cap → the SMALL geometry (oldest)
     * is evicted. With the skewed ledger the footprint read 99.5M +
     * 99.5M ≤ cap and nothing was evicted. */
    auto e = dsp.alloc_buffers(7, 2560, 1440, HAL_PIX_FMT_NV12, 15);
    assert(e.rc == DSP_SVC_OK);
    assert(g_last_pool_max == 18); /* max(retention_n=18, count=15) */
    for (uint64_t id : e.ids)
        assert(dsp.release_buffer(7, id) == DSP_SVC_OK);
    {
        const auto& st = dsp.stats();
        assert(st.retention_releases == 1); /* the small geometry     */
        assert(st.retention_parked == 30); /* 15 4K + 15 qhd         */
    }
    assert(g_release_count == 1);

    dsp.stop();
    dsp.release_client_buffers(7);
    assert(g_release_count == 31); /* + 30 flushed at stop           */
    std::printf("  case_mixed_chunks_ledger_exact ok\n");
}

static void case_floor_when_chunk_still_over_cap() {
    /* A geometry whose chunk exceeds the cap even at the floor (4)
     * keeps the floor chunk and simply doesn't park — functionally the
     * pre-fix behavior, with an eighth of the rebuild bytes. Small cap
     * stands in for a huge geometry (max_pixels_per_op caps w*h). */
    reset_globals();
    DspServiceConfig small_cfg;
    small_cfg.pool_retention_max_bytes = 1ULL << 20; /* 1MiB < 4*768KiB */
    DspService dsp(&g_dsp_ops, &g_fb_ops, small_cfg);
    assert(dsp.start());

    auto r1 = alloc_one(dsp, 7, 1024, 512);
    assert(g_last_pool_max == 4); /* floor, not 512KiB/768KiB=0         */
    assert(dsp.release_buffer(7, r1.ids[0]) == DSP_SVC_OK);
    assert(dsp.stats().retention_parked == 0); /* 3MiB chunk > 1MiB     */

    auto r2 = alloc_one(dsp, 7, 1024, 512);
    assert(g_request_count == 2); /* fresh HAL round — no park to reuse */
    assert(r2.fds[0] != r1.fds[0]);

    dsp.stop();
    dsp.release_client_buffers(7);
    std::printf("  case_floor_when_chunk_still_over_cap ok\n");
}

static void case_retention_off_keeps_ceiling() {
    /* Retention disabled: the fd ceiling (32) is asked for regardless —
     * pre-fix sizing, nothing to park into anyway. */
    reset_globals();
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 0;
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    (void)alloc_one(dsp, 7, 1024, 512);
    assert(g_last_pool_max == 32);

    dsp.stop();
    dsp.release_client_buffers(7);
    std::printf("  case_retention_off_keeps_ceiling ok\n");
}

int main() {
    case_reuse_hit();
    case_lazy_expiry();
    case_sweep_expiry();
    case_imports_never_parked();
    case_cap_evicts_oldest_geometry();
    case_disabled_by_config();
    case_stop_flushes_parks();
    case_retention_sizes_big_pool();
    case_src_dst_geometries_coexist();
    case_count_raises_chunk_over_cap();
    case_count_fitting_parks_and_evicts();
    case_reuse_parks_source_chunk();
    case_mixed_chunks_ledger_exact();
    case_floor_when_chunk_still_over_cap();
    case_retention_off_keeps_ceiling();
    std::printf("dsp_pool_retention_test: all assertions passed\n");
    return 0;
}
