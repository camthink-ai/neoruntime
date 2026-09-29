/**
 * @file dsp_import_write_target_test.cpp
 * @brief keep_fd read-only contract — guard test for the existing
 *        validate_and_pin import-as-write-target policy (NO production
 *        change; dsp_service.cpp already enforces this).
 *
 * The keep_fd/copy frame contract says frames handed to a client are
 * SOURCE-ONLY: the client draws on its OWN pool buffers, never back
 * into an imported frame. DspService enforces that at submit time:
 *
 *   - an imported buffer as the dst of RESIZE/CONVERT/... → rejected
 *     "imported buffers are source-only" (jobs_rejected++);
 *   - an imported buffer as the BLEND base → rejected "BLEND composites
 *     the base in place; imported frames cannot be the base";
 *   - the ONE exception: an imported ARGB32 overlay plane as a BLEND
 *     dst — the keep_fd draw recipe (own frame as overlay, pool NV12
 *     frame as base, 1:1 paste rect).
 *
 * Rejection is SYNCHRONOUS in submit_job (validate_and_pin fires before
 * queueing), so the fake HalDspOps only needs the sync resize/blend
 * pointers for the two ALLOWED cases; execute_job calls the sync ops,
 * never submit/wait.
 *
 * Host-only harness, same fake-op style as dsp_pool_retention_test.
 */

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/mman.h> /* memfd_create */
#include <unistd.h>   /* ftruncate, close */

#include "dsp_service.h"

/* ---------------- fake ops ---------------- */

static int fake_dsp_init(const HalDspConfig*, void** ctx) {
    *ctx = (void*)0x1;
    return 0;
}
static int fake_dsp_deinit(void*) { return 0; }

/* sync ops for the ALLOWED cases — execute_job calls these directly */
static int fake_resize(void*, const HalDspResizeParams*) { return 0; }
static int fake_blend(void*, const HalDspBlendParams*) { return 0; }

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
    for (uint32_t p = 0; p < fb->num_planes; ++p) fb->dma_fds[p] = 100 + p;
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

static HalDspOps g_dsp_ops = [] {
    HalDspOps ops = {};
    ops.init = fake_dsp_init;
    ops.deinit = fake_dsp_deinit;
    ops.resize = fake_resize;
    ops.blend = fake_blend;
    return ops;
}();
static HalFrameBufferOps g_fb_ops = [] {
    HalFrameBufferOps ops = {};
    ops.request_frame_buffer = fake_request_fb;
    ops.release_frame_buffer = fake_release_fb;
    return ops;
}();

/* memfd import (two-plane NV12 or single-plane ARGB32); asserts ok. */
static DspService::ImportResult import_memfd(DspService& dsp, int client_fd,
                                             uint32_t w, uint32_t h,
                                             HalPixelFormat fmt, uint32_t planes,
                                             const uint32_t* strides,
                                             const uint32_t* sizes,
                                             const char* tag) {
    int fd = memfd_create(tag, 0);
    assert(fd >= 0);
    uint32_t total = 0;
    for (uint32_t p = 0; p < planes; ++p) total += sizes[p];
    assert(ftruncate(fd, (off_t)total) == 0);
    std::vector<int> fds(planes);
    for (uint32_t p = 0; p < planes; ++p) fds[p] = fd;
    auto r = dsp.import_buffer(client_fd, w, h, fmt, planes, strides, sizes,
                               fds.data());
    close(fd);
    assert(r.rc == DSP_SVC_OK);
    return r;
}

/* pool NV12 alloc; asserts ok. */
static DspService::AllocResult alloc_nv12(DspService& dsp, int client_fd,
                                          uint32_t w, uint32_t h) {
    auto r = dsp.alloc_buffers(client_fd, w, h, HAL_PIX_FMT_NV12, 1);
    assert(r.rc == DSP_SVC_OK);
    assert(r.ids.size() == 1);
    return r;
}

/* ---------------- cases ---------------- */

static void case_import_dst_rejected() {
    /* An imported frame as the RESIZE dst = writing into the client's
     * keep_fd frame. Must be rejected synchronously, counted, and say
     * source-only. RESIZE takes no rects. */
    DspServiceConfig cfg;
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    const uint32_t strides[2] = {96, 96};
    const uint32_t sizes[2] = {96 * 64, 96 * 64 / 2};
    auto imp = import_memfd(dsp, 7, 96, 64, HAL_PIX_FMT_NV12, 2, strides,
                            sizes, "imp-dst");

    DspJobDesc desc;
    desc.op = HAL_DSP_OP_RESIZE;
    desc.src_id = imp.id; /* an imported src is FINE... */
    /* ...but point dst at the import too: the write target violation */
    desc.dst_ids = {imp.id};
    (void)dsp.submit_job(desc); /* sync submit: rejection is the result */

    const auto& st = dsp.stats();
    assert(st.jobs_rejected == 1);
    assert(st.jobs_ok == 0);

    dsp.stop();
    dsp.release_client_buffers(7);
    std::printf("  case_import_dst_rejected ok\n");
}

static void case_import_blend_base_rejected() {
    /* BLEND composites the base in place — an imported (keep_fd) frame
     * must never be the base. The base check fires before dst checks,
     * but supply a structurally valid ARGB32 dst + rect anyway. */
    DspServiceConfig cfg;
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    const uint32_t strides[2] = {96, 96};
    const uint32_t sizes[2] = {96 * 64, 96 * 64 / 2};
    auto imp = import_memfd(dsp, 7, 96, 64, HAL_PIX_FMT_NV12, 2, strides,
                            sizes, "imp-base");

    const uint32_t ostride[1] = {32 * 4}; /* ARGB32 min stride = w*4 */
    const uint32_t osizes[1] = {32 * 4 * 32};
    auto ovl = import_memfd(dsp, 7, 32, 32, HAL_PIX_FMT_ARGB32, 1, ostride,
                            osizes, "imp-ovl");

    DspJobDesc desc;
    desc.op = HAL_DSP_OP_BLEND;
    desc.src_id = imp.id; /* imported base — the violation */
    desc.dst_ids = {ovl.id};
    DspRect r{};
    r.width = 32;
    r.height = 32;
    r.dst_width = 32;
    r.dst_height = 32;
    desc.rects = {r};
    (void)dsp.submit_job(desc);

    const auto& st = dsp.stats();
    assert(st.jobs_rejected == 1);
    assert(st.jobs_ok == 0);

    dsp.stop();
    dsp.release_client_buffers(7);
    std::printf("  case_import_blend_base_rejected ok\n");
}

static void case_import_blend_overlay_allowed() {
    /* The keep_fd draw recipe, and the ONLY import write path: pool NV12
     * base + imported ARGB32 overlay dst, 1:1 paste rect → blend runs. */
    DspServiceConfig cfg;
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    auto base = alloc_nv12(dsp, 7, 32, 32);

    /* 16x16: import width/height floor is [16, 8192] */
    const uint32_t ostride[1] = {16 * 4}; /* ARGB32 min stride = w*4 */
    const uint32_t osizes[1] = {16 * 4 * 16};
    auto ovl = import_memfd(dsp, 7, 16, 16, HAL_PIX_FMT_ARGB32, 1, ostride,
                            osizes, "imp-ovl-ok");

    DspJobDesc desc;
    desc.op = HAL_DSP_OP_BLEND;
    desc.src_id = base.ids[0]; /* pool base — composites in place */
    desc.dst_ids = {ovl.id};
    DspRect r{};
    r.x = 4;
    r.y = 4;
    r.width = 16;
    r.height = 16;
    r.dst_width = 16;
    r.dst_height = 16;
    desc.rects = {r};

    auto res = dsp.submit_job(desc);
    assert(res.rc == DSP_SVC_OK);
    assert(res.message == "blend");
    const auto& st = dsp.stats();
    assert(st.jobs_ok == 1);
    assert(st.jobs_rejected == 0);

    dsp.stop();
    dsp.release_client_buffers(7);
    std::printf("  case_import_blend_overlay_allowed ok\n");
}

static void case_pool_resize_no_regression() {
    /* Pool → pool stays the normal path: RESIZE, no rects, ok. */
    DspServiceConfig cfg;
    DspService dsp(&g_dsp_ops, &g_fb_ops, cfg);
    assert(dsp.start());

    auto src = alloc_nv12(dsp, 7, 32, 32);
    auto dst = alloc_nv12(dsp, 7, 16, 16);

    DspJobDesc desc;
    desc.op = HAL_DSP_OP_RESIZE;
    desc.src_id = src.ids[0];
    desc.dst_ids = {dst.ids[0]};

    auto res = dsp.submit_job(desc);
    assert(res.rc == DSP_SVC_OK);
    assert(res.message == "resize");
    const auto& st = dsp.stats();
    assert(st.jobs_ok == 1);
    assert(st.jobs_rejected == 0);

    dsp.stop();
    dsp.release_client_buffers(7);
    std::printf("  case_pool_resize_no_regression ok\n");
}

int main() {
    case_import_dst_rejected();
    case_import_blend_base_rejected();
    case_import_blend_overlay_allowed();
    case_pool_resize_no_regression();
    std::printf("dsp_import_write_target_test: all assertions passed\n");
    return 0;
}
