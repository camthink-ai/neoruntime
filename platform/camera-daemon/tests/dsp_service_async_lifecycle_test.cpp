#include "dsp_service.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {

std::atomic<int> g_releases{0};
std::atomic<int> g_resizes{0};
std::mutex g_resize_mu;
std::condition_variable g_resize_cv;

int dsp_init(const HalDspConfig*, void** ctx) {
    *ctx = reinterpret_cast<void*>(0x1);
    return 0;
}

int dsp_deinit(void*) { return 0; }

int dsp_resize(void*, const HalDspResizeParams* params) {
    assert(params);
    assert(params->src);
    assert(params->dst);
    assert(params->src->width == 64);
    assert(params->dst->width == 32);
    g_resizes.fetch_add(1);
    g_resize_cv.notify_all();
    return 0;
}

int request_buffer(const HalFrameBufferRequest* req, HalFrameBuffer** out) {
    auto* fb = new HalFrameBuffer{};
    fb->width = req->width;
    fb->height = req->height;
    fb->format = req->format;
    fb->mem_type = req->mem_type;
    fb->num_planes = req->format == HAL_PIX_FMT_NV12 ? 2 : 1;
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

int release_buffer(HalFrameBuffer* fb) {
    delete fb;
    g_releases.fetch_add(1);
    return 0;
}

bool wait_for_resize() {
    std::unique_lock<std::mutex> lk(g_resize_mu);
    return g_resize_cv.wait_for(lk, std::chrono::seconds(2), [] {
        return g_resizes.load() == 1;
    });
}

} // namespace

int main() {
    HalDspOps dsp{};
    dsp.init = dsp_init;
    dsp.deinit = dsp_deinit;
    dsp.resize = dsp_resize;

    HalFrameBufferOps fb{};
    fb.request_frame_buffer = request_buffer;
    fb.release_frame_buffer = release_buffer;

    DspServiceConfig cfg;
    cfg.max_async_jobs_per_client = 2;
    cfg.quota_jobs_per_sec = 1000;
    cfg.quota_mpix_per_sec = 1000;

    DspService service(&dsp, &fb, cfg);
    assert(service.start());

    constexpr int owner = 41;
    auto src = service.alloc_buffers(owner, 64, 64, HAL_PIX_FMT_NV12, 1);
    auto dst = service.alloc_buffers(owner, 32, 32, HAL_PIX_FMT_NV12, 1);
    assert(src.rc == DSP_SVC_OK && src.ids.size() == 1);
    assert(dst.rc == DSP_SVC_OK && dst.ids.size() == 1);

    std::mutex hook_mu;
    std::condition_variable hook_cv;
    bool registered = false;
    bool allow_enqueue = false;
    service.set_after_async_register_hook([&] {
        std::unique_lock<std::mutex> lk(hook_mu);
        registered = true;
        hook_cv.notify_all();
        hook_cv.wait(lk, [&] { return allow_enqueue; });
    });

    DspJobDesc desc;
    desc.op = HAL_DSP_OP_RESIZE;
    desc.src_id = src.ids[0];
    desc.dst_ids.push_back(dst.ids[0]);

    uint64_t job_id = 0;
    DspJobResult submit_result;
    std::thread submitter([&] {
        submit_result = service.submit_job_async(desc, job_id);
    });

    {
        std::unique_lock<std::mutex> lk(hook_mu);
        assert(hook_cv.wait_for(lk, std::chrono::seconds(2), [&] {
            return registered;
        }));
    }
    assert(service.async_job_count_for_test() == 1);
    assert(service.client_async_job_count_for_test(owner) == 1);

    // This disconnect occurs after registration but before enqueue. It must
    // reap the registry/count while leaving pins for the future worker.
    service.release_client_buffers(owner);
    assert(service.async_job_count_for_test() == 0);
    assert(service.client_async_job_count_for_test(owner) == 0);
    assert(g_releases.load() == 0);

    {
        std::lock_guard<std::mutex> lk(hook_mu);
        allow_enqueue = true;
    }
    hook_cv.notify_all();
    submitter.join();
    assert(submit_result.rc == DSP_SVC_OK);
    assert(job_id != 0);

    assert(wait_for_resize());
    for (int i = 0; i < 200 && g_releases.load() != 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(g_releases.load() == 2);
    assert(service.async_job_count_for_test() == 0);
    assert(service.client_async_job_count_for_test(owner) == 0);

    service.stop();
    std::puts("dsp_service_async_lifecycle_test: PASS");
    return 0;
}
