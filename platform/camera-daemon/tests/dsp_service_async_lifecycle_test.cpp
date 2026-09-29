#include "dsp_service.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <new>
#include <string>
#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace {

std::atomic<int> g_releases{0};
std::atomic<int> g_resizes{0};
std::atomic<int> g_request_fail_after{-1};
std::atomic<int> g_buffer_requests{0};
std::atomic<uint32_t> g_plane_extra_bytes{0};
std::atomic<uint32_t> g_last_pool_max_buffers{0};
std::mutex g_resize_block_mu;
std::condition_variable g_resize_block_cv;
bool g_block_resize = false;
bool g_resize_entered = false;
bool g_allow_resize = false;

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
    {
        std::unique_lock<std::mutex> lock(g_resize_block_mu);
        if (g_block_resize) {
            g_resize_entered = true;
            g_resize_block_cv.notify_all();
            g_resize_block_cv.wait(lock, [] { return g_allow_resize; });
        }
    }
    g_resizes.fetch_add(1);
    return 0;
}

int request_buffer(const HalFrameBufferRequest* req, HalFrameBuffer** out) {
    g_buffer_requests.fetch_add(1);
    g_last_pool_max_buffers.store(req->pool_max_buffers);
    int fail_after = g_request_fail_after.load();
    if (fail_after == 0) return -1;
    if (fail_after > 0) g_request_fail_after.fetch_sub(1);

    auto* fb = new HalFrameBuffer{};
    fb->width = req->width;
    fb->height = req->height;
    fb->format = req->format;
    fb->mem_type = req->mem_type;
    fb->num_planes = req->format == HAL_PIX_FMT_NV12 ? 2 : 1;
    for (uint32_t p = 0; p < HAL_MAX_PLANES; ++p) fb->dma_fds[p] = -1;
    const uint32_t bytes_per_pixel =
        req->format == HAL_PIX_FMT_RGB24
            ? 3
            : (req->format == HAL_PIX_FMT_ARGB32 ? 4 : 1);
    fb->strides[0] = req->width * bytes_per_pixel;
    fb->sizes[0] = req->width * req->height * bytes_per_pixel +
                   g_plane_extra_bytes.load();
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

void arm_resize_block() {
    std::lock_guard<std::mutex> lock(g_resize_block_mu);
    g_block_resize = true;
    g_resize_entered = false;
    g_allow_resize = false;
}

void wait_for_resize_entry() {
    std::unique_lock<std::mutex> lock(g_resize_block_mu);
    assert(g_resize_block_cv.wait_for(lock, std::chrono::seconds(2), [] {
        return g_resize_entered;
    }));
}

void release_resize_block() {
    {
        std::lock_guard<std::mutex> lock(g_resize_block_mu);
        g_allow_resize = true;
        g_block_resize = false;
    }
    g_resize_block_cv.notify_all();
}

DspJobDesc allocate_resize_job(DspService& service, int owner) {
    auto src = service.alloc_buffers(owner, 64, 64, HAL_PIX_FMT_NV12, 1);
    auto dst = service.alloc_buffers(owner, 32, 32, HAL_PIX_FMT_NV12, 1);
    assert(src.rc == DSP_SVC_OK && src.ids.size() == 1);
    assert(dst.rc == DSP_SVC_OK && dst.ids.size() == 1);

    DspJobDesc desc;
    desc.op = HAL_DSP_OP_RESIZE;
    desc.src_id = src.ids[0];
    desc.dst_ids.push_back(dst.ids[0]);
    return desc;
}

int create_memfd(size_t size) {
    int fd = static_cast<int>(::syscall(
        SYS_memfd_create, "dsp-service-test", MFD_CLOEXEC));
    assert(fd >= 0);
    assert(::ftruncate(fd, static_cast<off_t>(size)) == 0);
    return fd;
}

DspService::ImportResult import_nv12(DspService& service, int owner, int fd,
                                     uint32_t width = 32,
                                     uint32_t height = 32) {
    const uint32_t strides[2] = {width, width};
    const uint32_t sizes[2] = {width * height, width * height / 2};
    const int fds[2] = {fd, fd};
    return service.import_buffer(owner, width, height, HAL_PIX_FMT_NV12, 2,
                                 strides, sizes, fds);
}

void test_stop_fences_async_submission(HalDspOps* dsp, HalFrameBufferOps* fb,
                                       const DspServiceConfig& cfg,
                                       bool stop_after_registration) {
    DspService service(dsp, fb, cfg);
    assert(service.start());

    constexpr int owner = 73;
    const auto desc = allocate_resize_job(service, owner);
    const int releases_before = g_releases.load();

    std::mutex hook_mu;
    std::condition_variable hook_cv;
    bool hook_entered = false;
    bool release_hook = false;
    auto hook = [&] {
        std::unique_lock<std::mutex> lk(hook_mu);
        hook_entered = true;
        hook_cv.notify_all();
        hook_cv.wait(lk, [&] { return release_hook; });
    };
    if (stop_after_registration)
        service.set_after_async_register_hook(hook);
    else
        service.set_after_async_pin_hook(hook);

    uint64_t job_id = 0;
    DspJobResult submit_result;
    std::thread submitter([&] {
        submit_result = service.submit_job_async(desc, job_id);
    });
    {
        std::unique_lock<std::mutex> lk(hook_mu);
        assert(hook_cv.wait_for(lk, std::chrono::seconds(2), [&] {
            return hook_entered;
        }));
    }

    std::atomic<bool> stop_done{false};
    std::thread stopper([&] {
        service.stop();
        stop_done.store(true, std::memory_order_release);
    });

    DspJobDesc invalid_desc;
    uint64_t rejected_job_id = 0;
    DspJobResult rejected;
    for (int i = 0; i < 200; ++i) {
        rejected = service.submit_job_async(invalid_desc, rejected_job_id);
        if (rejected.rc == DSP_SVC_ERR_UNAVAILABLE) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(rejected.rc == DSP_SVC_ERR_UNAVAILABLE);
    assert(rejected_job_id == 0);
    assert(!stop_done.load(std::memory_order_acquire));
    assert(g_releases.load() == releases_before);

    {
        std::lock_guard<std::mutex> lk(hook_mu);
        release_hook = true;
    }
    hook_cv.notify_all();
    submitter.join();
    stopper.join();

    assert(submit_result.rc == DSP_SVC_OK);
    assert(job_id != 0);
    assert(stop_done.load(std::memory_order_acquire));
    assert(service.async_job_count_for_test() == 0);
    assert(service.client_async_job_count_for_test(owner) == 0);
    assert(g_releases.load() == releases_before + 2);
}

void test_buffer_resource_caps(HalDspOps* dsp, HalFrameBufferOps* fb) {
    DspServiceConfig cfg;
    // fork-only P1-9 retention parks released pool buffers for 3s by
    // default; this suite asserts immediate HAL release timing, so every
    // config here disables it via the documented rollback knob.
    cfg.pool_retention_ms = 0;
    cfg.max_buffers_per_client = 2;
    cfg.max_client_pixels = 49152;
    cfg.max_total_buffers = 2;
    cfg.max_total_buffer_pixels = 49152;

    DspService service(dsp, fb, cfg);
    assert(service.start());

    int conn_a[2] = {-1, -1};
    int conn_b[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, conn_a) == 0);
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, conn_b) == 0);

    auto first = service.alloc_buffers(
        conn_a[0], 32, 32, HAL_PIX_FMT_NV12, 2);
    assert(first.rc == DSP_SVC_OK && first.ids.size() == 2);

    auto process_limited = service.alloc_buffers(
        conn_b[0], 32, 32, HAL_PIX_FMT_NV12, 1);
    assert(process_limited.rc == DSP_SVC_ERR_LIMIT);
    assert(process_limited.message.find("per-process") != std::string::npos);

    service.release_client_buffers(conn_a[0]);
    auto after_release = service.alloc_buffers(
        conn_b[0], 32, 32, HAL_PIX_FMT_NV12, 1);
    assert(after_release.rc == DSP_SVC_OK);
    service.release_client_buffers(conn_b[0]);
    service.stop();

    for (int fd : conn_a) ::close(fd);
    for (int fd : conn_b) ::close(fd);

    DspServiceConfig total_cfg;
    total_cfg.pool_retention_ms = 0;
    total_cfg.max_buffers_per_client = 4;
    total_cfg.max_client_pixels = 98304;
    total_cfg.max_total_buffers = 2;
    total_cfg.max_total_buffer_pixels = 49152;

    DspService total_service(dsp, fb, total_cfg);
    assert(total_service.start());
    int total_conn[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, total_conn) == 0);
    auto total_first = total_service.alloc_buffers(
        total_conn[0], 32, 32, HAL_PIX_FMT_NV12, 2);
    assert(total_first.rc == DSP_SVC_OK);
    auto total_limited = total_service.alloc_buffers(
        total_conn[0], 32, 32, HAL_PIX_FMT_NV12, 1);
    assert(total_limited.rc == DSP_SVC_ERR_LIMIT);
    assert(total_limited.message.find("service buffer") != std::string::npos);
    total_service.release_client_buffers(total_conn[0]);
    total_service.stop();
    for (int fd : total_conn) ::close(fd);
}

void test_allocation_reservation_rollback(HalDspOps* dsp,
                                          HalFrameBufferOps* fb) {
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 0;
    cfg.max_buffers_per_client = 1;
    cfg.max_client_pixels = 49152;
    cfg.max_total_buffers = 1;
    cfg.max_total_buffer_pixels = 49152;

    DspService service(dsp, fb, cfg);
    assert(service.start());

    g_request_fail_after.store(0);
    auto failed = service.alloc_buffers(101, 32, 32, HAL_PIX_FMT_NV12, 1);
    assert(failed.rc == DSP_SVC_ERR_NO_MEM);

    g_request_fail_after.store(-1);
    auto retried = service.alloc_buffers(101, 32, 32, HAL_PIX_FMT_NV12, 1);
    assert(retried.rc == DSP_SVC_OK);
    service.release_client_buffers(101);
    service.stop();
}

void test_import_resource_caps(HalDspOps* dsp, HalFrameBufferOps* fb) {
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 0;
    cfg.max_imports_per_client = 2;
    cfg.max_import_bytes_per_client = 1536;
    cfg.max_total_imports = 4;
    cfg.max_total_import_bytes = 6144;

    DspService service(dsp, fb, cfg);
    assert(service.start());

    int conn_a[2] = {-1, -1};
    int conn_b[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, conn_a) == 0);
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, conn_b) == 0);
    const int fd = create_memfd(1536);

    auto first = import_nv12(service, conn_a[0], fd);
    assert(first.rc == DSP_SVC_OK && first.id != 0);
    auto byte_limited = import_nv12(service, conn_b[0], fd);
    assert(byte_limited.rc == DSP_SVC_ERR_LIMIT);
    assert(byte_limited.message.find("per-process import byte") !=
           std::string::npos);

    assert(service.release_buffer(conn_a[0], first.id) == DSP_SVC_OK);
    auto after_release = import_nv12(service, conn_b[0], fd);
    assert(after_release.rc == DSP_SVC_OK);
    assert(service.release_buffer(conn_b[0], after_release.id) == DSP_SVC_OK);

    const uint32_t odd_strides[2] = {33, 33};
    const uint32_t odd_sizes[2] = {33 * 32, 33 * 16};
    const int odd_fds[2] = {fd, fd};
    auto odd = service.import_buffer(conn_a[0], 33, 32, HAL_PIX_FMT_NV12, 2,
                                     odd_strides, odd_sizes, odd_fds);
    assert(odd.rc == DSP_SVC_ERR_INVALID);
    assert(odd.message.find("must be even") != std::string::npos);

    service.stop();
    ::close(fd);
    for (int socket_fd : conn_a) ::close(socket_fd);
    for (int socket_fd : conn_b) ::close(socket_fd);

    DspServiceConfig total_cfg;
    total_cfg.pool_retention_ms = 0;
    total_cfg.max_imports_per_client = 4;
    total_cfg.max_import_bytes_per_client = 6144;
    total_cfg.max_total_imports = 1;
    total_cfg.max_total_import_bytes = 1536;

    DspService total_service(dsp, fb, total_cfg);
    assert(total_service.start());
    int total_conn[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, total_conn) == 0);
    const int total_fd = create_memfd(1536);
    auto total_first = import_nv12(total_service, total_conn[0], total_fd);
    assert(total_first.rc == DSP_SVC_OK);
    auto total_limited = import_nv12(total_service, total_conn[0], total_fd);
    assert(total_limited.rc == DSP_SVC_ERR_LIMIT);
    assert(total_limited.message.find("service import count") !=
           std::string::npos);
    assert(total_service.release_buffer(total_conn[0], total_first.id) ==
           DSP_SVC_OK);
    total_service.stop();
    ::close(total_fd);
    for (int socket_fd : total_conn) ::close(socket_fd);
}

void test_import_reservation_rollback(HalDspOps* dsp,
                                      HalFrameBufferOps* fb) {
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 0;
    cfg.max_imports_per_client = 1;
    cfg.max_import_bytes_per_client = 1536;
    cfg.max_total_imports = 1;
    cfg.max_total_import_bytes = 1536;

    DspService service(dsp, fb, cfg);
    assert(service.start());

    const int fd = create_memfd(512);
    auto short_import = import_nv12(service, 111, fd);
    assert(short_import.rc == DSP_SVC_ERR_INVALID);

    assert(::ftruncate(fd, 1536) == 0);
    auto retried = import_nv12(service, 111, fd);
    assert(retried.rc == DSP_SVC_OK);
    assert(service.release_buffer(111, retried.id) == DSP_SVC_OK);

    service.stop();
    ::close(fd);
}

void test_detached_pins_remain_charged(HalDspOps* dsp,
                                       HalFrameBufferOps* fb) {
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 0;
    cfg.max_buffers_per_client = 2;
    cfg.max_client_pixels = 245760;
    cfg.max_total_buffers = 2;
    cfg.max_total_buffer_pixels = 245760;
    cfg.quota_jobs_per_sec = 1000;
    cfg.quota_mpix_per_sec = 1000;

    DspService service(dsp, fb, cfg);
    assert(service.start());

    int conn_a[2] = {-1, -1};
    int conn_b[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, conn_a) == 0);
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, conn_b) == 0);

    const auto desc = allocate_resize_job(service, conn_a[0]);
    const int releases_before = g_releases.load();
    std::mutex hook_mu;
    std::condition_variable hook_cv;
    bool registered = false;
    bool allow_enqueue = false;
    service.set_after_async_register_hook([&] {
        std::unique_lock<std::mutex> lock(hook_mu);
        registered = true;
        hook_cv.notify_all();
        hook_cv.wait(lock, [&] { return allow_enqueue; });
    });

    uint64_t job_id = 0;
    DspJobResult submit_result;
    std::thread submitter([&] {
        submit_result = service.submit_job_async(desc, job_id);
    });
    {
        std::unique_lock<std::mutex> lock(hook_mu);
        assert(hook_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return registered;
        }));
    }

    service.release_client_buffers(conn_a[0]);
    auto while_pinned = service.alloc_buffers(
        conn_b[0], 32, 32, HAL_PIX_FMT_NV12, 1);
    assert(while_pinned.rc == DSP_SVC_ERR_LIMIT);
    // Admission control rejects before HAL, while the disconnected job still
    // owns both detached pins until the blocked submitter rolls back.
    assert(g_releases.load() == releases_before);

    {
        std::lock_guard<std::mutex> lock(hook_mu);
        allow_enqueue = true;
    }
    hook_cv.notify_all();
    submitter.join();
    assert(submit_result.rc == DSP_SVC_ERR_NO_BUFFER);

    for (int i = 0; i < 200 && g_releases.load() != releases_before + 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(g_releases.load() == releases_before + 2);

    auto after_unpin = service.alloc_buffers(
        conn_b[0], 32, 32, HAL_PIX_FMT_NV12, 1);
    assert(after_unpin.rc == DSP_SVC_OK);
    service.release_client_buffers(conn_b[0]);
    service.stop();

    for (int fd : conn_a) ::close(fd);
    for (int fd : conn_b) ::close(fd);
}

void test_stop_waits_for_buffer_pin(HalDspOps* dsp, HalFrameBufferOps* fb) {
    DspService service(dsp, fb);
    assert(service.start());

    constexpr int owner = 201;
    auto allocated = service.alloc_buffers(owner, 32, 32, HAL_PIX_FMT_NV12, 1);
    assert(allocated.rc == DSP_SVC_OK);
    auto pin = service.pin_buffer(allocated.ids[0]);
    assert(pin.ok() && pin.fb());
    const int releases_before = g_releases.load();
    assert(service.release_buffer(owner, allocated.ids[0]) == DSP_SVC_OK);

    std::atomic<bool> stop_done{false};
    std::thread stopper([&] {
        service.stop();
        stop_done.store(true, std::memory_order_release);
    });

    DspService::BufferPin rejected;
    for (int i = 0; i < 200; ++i) {
        rejected = service.pin_buffer(allocated.ids[0]);
        if (rejected.rc() == DSP_SVC_ERR_UNAVAILABLE) break;
        std::this_thread::yield();
    }
    assert(rejected.rc() == DSP_SVC_ERR_UNAVAILABLE);
    assert(!stop_done.load(std::memory_order_acquire));
    assert(g_releases.load() == releases_before);

    pin = DspService::BufferPin{};
    stopper.join();
    assert(stop_done.load(std::memory_order_acquire));
    assert(g_releases.load() == releases_before + 1);
}

void test_buffer_registration_fenced_by_disconnect(HalDspOps* dsp,
                                                   HalFrameBufferOps* fb) {
    DspService service(dsp, fb);
    assert(service.start());

    int conn[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, conn) == 0);
    const int owner = conn[0];
    const int releases_before = g_releases.load();

    std::mutex hook_mu;
    std::condition_variable hook_cv;
    bool hook_entered = false;
    bool release_hook = false;
    std::atomic<int> hook_calls{0};
    service.set_before_buffer_register_hook([&] {
        if (hook_calls.fetch_add(1) != 0) return;
        std::unique_lock<std::mutex> lock(hook_mu);
        hook_entered = true;
        hook_cv.notify_all();
        hook_cv.wait(lock, [&] { return release_hook; });
    });

    DspService::AllocResult delayed;
    std::thread allocator([&] {
        delayed = service.alloc_buffers(owner, 32, 32, HAL_PIX_FMT_NV12, 1);
    });
    {
        std::unique_lock<std::mutex> lock(hook_mu);
        assert(hook_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return hook_entered;
        }));
    }

    service.release_client_buffers(owner);
    auto disconnected =
        service.alloc_buffers(owner, 32, 32, HAL_PIX_FMT_NV12, 1);
    assert(disconnected.rc == DSP_SVC_ERR_NO_BUFFER);

    {
        std::lock_guard<std::mutex> lock(hook_mu);
        release_hook = true;
    }
    hook_cv.notify_all();
    allocator.join();
    assert(delayed.rc == DSP_SVC_ERR_NO_BUFFER);
    assert(delayed.ids.empty());
    assert(g_releases.load() == releases_before + 1);

    ::close(conn[0]);
    ::close(conn[1]);
    int replacement[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, replacement) == 0);
    if (replacement[0] != owner) {
        assert(::dup2(replacement[0], owner) == owner);
        ::close(replacement[0]);
        replacement[0] = owner;
    }
    auto reused = service.alloc_buffers(owner, 32, 32, HAL_PIX_FMT_NV12, 1);
    assert(reused.rc == DSP_SVC_OK);
    service.release_client_buffers(owner);
    service.stop();
    ::close(replacement[0]);
    ::close(replacement[1]);
}

void test_buffer_registration_fenced_by_stop(HalDspOps* dsp,
                                             HalFrameBufferOps* fb) {
    DspService service(dsp, fb);
    assert(service.start());

    std::mutex hook_mu;
    std::condition_variable hook_cv;
    bool hook_entered = false;
    bool release_hook = false;
    service.set_before_buffer_register_hook([&] {
        std::unique_lock<std::mutex> lock(hook_mu);
        hook_entered = true;
        hook_cv.notify_all();
        hook_cv.wait(lock, [&] { return release_hook; });
    });

    DspService::AllocResult delayed;
    std::thread allocator([&] {
        delayed = service.alloc_buffers(211, 32, 32, HAL_PIX_FMT_NV12, 1);
    });
    {
        std::unique_lock<std::mutex> lock(hook_mu);
        assert(hook_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return hook_entered;
        }));
    }

    std::atomic<bool> stop_done{false};
    std::thread stopper([&] {
        service.stop();
        stop_done.store(true, std::memory_order_release);
    });
    for (int i = 0; i < 200 && service.is_running(); ++i)
        std::this_thread::yield();
    assert(!service.is_running());
    assert(!stop_done.load(std::memory_order_acquire));

    {
        std::lock_guard<std::mutex> lock(hook_mu);
        release_hook = true;
    }
    hook_cv.notify_all();
    allocator.join();
    stopper.join();
    assert(delayed.rc == DSP_SVC_ERR_UNAVAILABLE);
    assert(delayed.ids.empty());
}

void test_import_registration_fenced_by_disconnect(HalDspOps* dsp,
                                                   HalFrameBufferOps* fb) {
    DspService service(dsp, fb);
    assert(service.start());

    int conn[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, conn) == 0);
    const int fd = create_memfd(1536);
    std::mutex hook_mu;
    std::condition_variable hook_cv;
    bool hook_entered = false;
    bool release_hook = false;
    service.set_before_buffer_register_hook([&] {
        std::unique_lock<std::mutex> lock(hook_mu);
        hook_entered = true;
        hook_cv.notify_all();
        hook_cv.wait(lock, [&] { return release_hook; });
    });

    DspService::ImportResult delayed;
    std::thread importer([&] {
        delayed = import_nv12(service, conn[0], fd);
    });
    {
        std::unique_lock<std::mutex> lock(hook_mu);
        assert(hook_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return hook_entered;
        }));
    }
    service.release_client_buffers(conn[0]);
    {
        std::lock_guard<std::mutex> lock(hook_mu);
        release_hook = true;
    }
    hook_cv.notify_all();
    importer.join();
    assert(delayed.rc == DSP_SVC_ERR_NO_BUFFER);
    assert(delayed.id == 0);

    service.stop();
    ::close(fd);
    for (int socket_fd : conn) ::close(socket_fd);
}

void test_pin_bookkeeping_allocation_failures(HalDspOps* dsp,
                                              HalFrameBufferOps* fb) {
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 0;
    cfg.quota_jobs_per_sec = 1000;
    cfg.quota_mpix_per_sec = 1000;
    DspService service(dsp, fb, cfg);
    assert(service.start());

    constexpr int owner = 221;
    const auto desc = allocate_resize_job(service, owner);
    const int releases_before = g_releases.load();

    service.set_before_pin_bookkeeping_hook([] { throw std::bad_alloc(); });
    uint64_t job_id = 0;
    DspJobResult before_pin = service.submit_job_async(desc, job_id);
    assert(before_pin.rc == DSP_SVC_ERR_NO_MEM);
    assert(job_id == 0);

    service.set_before_pin_bookkeeping_hook({});
    service.set_after_async_pin_hook([] { throw std::bad_alloc(); });
    DspJobResult after_pin = service.submit_job_async(desc, job_id);
    assert(after_pin.rc == DSP_SVC_ERR_NO_MEM);
    assert(job_id == 0);
    service.set_after_async_pin_hook({});

    service.release_client_buffers(owner);
    service.stop();
    assert(g_releases.load() == releases_before + 2);
}

void test_random_id_failure_is_transactional(HalDspOps* dsp,
                                             HalFrameBufferOps* fb) {
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 0;
    cfg.quota_jobs_per_sec = 1000;
    cfg.quota_mpix_per_sec = 1000;
    DspService service(dsp, fb, cfg);
    assert(service.start());

    constexpr int import_owner = 225;
    const int fd = create_memfd(1536);
    service.set_random_id_failure_for_test(true);
    const auto failed_import = import_nv12(service, import_owner, fd);
    assert(failed_import.rc == DSP_SVC_ERR_NO_MEM);
    assert(failed_import.id == 0);

    service.set_random_id_failure_for_test(false);
    const auto imported = import_nv12(service, import_owner, fd);
    assert(imported.rc == DSP_SVC_OK);
    assert(imported.id != 0);
    service.release_client_buffers(import_owner);
    ::close(fd);

    constexpr int job_owner = 226;
    const auto desc = allocate_resize_job(service, job_owner);
    uint64_t job_id = 0;
    service.set_random_id_failure_for_test(true);
    const auto failed_submit = service.submit_job_async(desc, job_id);
    assert(failed_submit.rc == DSP_SVC_ERR_NO_MEM);
    assert(job_id == 0);
    assert(service.async_job_count_for_test() == 0);
    assert(service.async_owner_slot_count_for_test() == 0);
    assert(service.client_async_job_count_for_test(job_owner) == 0);

    service.set_random_id_failure_for_test(false);
    const auto submitted = service.submit_job_async(desc, job_id);
    assert(submitted.rc == DSP_SVC_OK);
    assert(job_id != 0);
    bool done = false;
    const auto waited = service.wait_job(job_id, 2000, done);
    assert(done);
    assert(waited.rc == DSP_SVC_OK);

    service.release_client_buffers(job_owner);
    service.stop();
}

void test_async_waiters_are_bounded(HalDspOps* dsp,
                                    HalFrameBufferOps* fb) {
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 0;
    cfg.quota_jobs_per_sec = 1000;
    cfg.quota_mpix_per_sec = 1000;
    cfg.max_waiters_per_job = 1;
    cfg.max_total_waiters = 1;
    cfg.max_wait_job_timeout_ms = 25;
    DspService service(dsp, fb, cfg);
    assert(service.start());

    constexpr int owner = 227;
    const auto first_desc = allocate_resize_job(service, owner);
    const auto second_desc = allocate_resize_job(service, owner);
    arm_resize_block();
    uint64_t first_job_id = 0;
    uint64_t second_job_id = 0;
    assert(service.submit_job_async(first_desc, first_job_id).rc == DSP_SVC_OK);
    assert(service.submit_job_async(second_desc, second_job_id).rc == DSP_SVC_OK);
    wait_for_resize_entry();

    std::mutex hook_mu;
    std::condition_variable hook_cv;
    bool waiter_registered = false;
    bool release_waiter = false;
    service.set_after_wait_job_lookup_hook([&] {
        std::unique_lock<std::mutex> lock(hook_mu);
        waiter_registered = true;
        hook_cv.notify_all();
        hook_cv.wait(lock, [&] { return release_waiter; });
    });

    bool first_done = false;
    DspJobResult first_wait;
    std::thread waiter([&] {
        first_wait = service.wait_job(first_job_id, 1000, first_done);
    });
    {
        std::unique_lock<std::mutex> lock(hook_mu);
        assert(hook_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return waiter_registered;
        }));
    }

    bool excess_done = false;
    const auto per_job_excess = service.wait_job(first_job_id, 1000,
                                                 excess_done);
    assert(!excess_done);
    assert(per_job_excess.rc == DSP_SVC_ERR_QUOTA);
    const auto global_excess = service.wait_job(second_job_id, 1000,
                                                excess_done);
    assert(!excess_done);
    assert(global_excess.rc == DSP_SVC_ERR_QUOTA);

    {
        std::lock_guard<std::mutex> lock(hook_mu);
        release_waiter = true;
    }
    hook_cv.notify_all();
    release_resize_block();
    waiter.join();
    service.set_after_wait_job_lookup_hook({});
    assert(first_done);
    assert(first_wait.rc == DSP_SVC_OK);

    bool second_done = false;
    const auto second_wait = service.wait_job(second_job_id, 1000, second_done);
    assert(second_done);
    assert(second_wait.rc == DSP_SVC_OK);

    const auto third_desc = allocate_resize_job(service, owner);
    arm_resize_block();
    uint64_t third_job_id = 0;
    assert(service.submit_job_async(third_desc, third_job_id).rc == DSP_SVC_OK);
    wait_for_resize_entry();
    const auto started = std::chrono::steady_clock::now();
    bool third_done = false;
    const auto clamped = service.wait_job(third_job_id, UINT32_MAX, third_done);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    assert(!third_done);
    assert(clamped.rc == DSP_SVC_ERR_TIMEOUT);
    assert(elapsed < std::chrono::seconds(1));
    release_resize_block();
    const auto third_wait = service.wait_job(third_job_id, 1000, third_done);
    assert(third_done);
    assert(third_wait.rc == DSP_SVC_OK);

    service.release_client_buffers(owner);
    service.stop();
}

void test_stop_wakes_waiters_for_queued_jobs(HalDspOps* dsp,
                                              HalFrameBufferOps* fb) {
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 0;
    cfg.quota_jobs_per_sec = 1000;
    cfg.quota_mpix_per_sec = 1000;
    cfg.max_wait_job_timeout_ms = 1000;
    DspService service(dsp, fb, cfg);
    assert(service.start());

    constexpr int owner = 228;
    const auto running_desc = allocate_resize_job(service, owner);
    const auto queued_desc = allocate_resize_job(service, owner);
    arm_resize_block();
    uint64_t running_job_id = 0;
    uint64_t queued_job_id = 0;
    assert(service.submit_job_async(running_desc, running_job_id).rc == DSP_SVC_OK);
    assert(service.submit_job_async(queued_desc, queued_job_id).rc == DSP_SVC_OK);
    wait_for_resize_entry();

    std::mutex hook_mu;
    std::condition_variable hook_cv;
    bool waiter_registered = false;
    bool release_waiter = false;
    service.set_after_wait_job_lookup_hook([&] {
        std::unique_lock<std::mutex> lock(hook_mu);
        waiter_registered = true;
        hook_cv.notify_all();
        hook_cv.wait(lock, [&] { return release_waiter; });
    });

    bool done = false;
    DspJobResult waited;
    std::thread waiter([&] {
        waited = service.wait_job(queued_job_id, 1000, done);
    });
    {
        std::unique_lock<std::mutex> lock(hook_mu);
        assert(hook_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return waiter_registered;
        }));
    }

    std::mutex stop_mu;
    std::condition_variable stop_cv;
    bool stop_done = false;
    std::thread stopper([&] {
        service.stop();
        {
            std::lock_guard<std::mutex> lock(stop_mu);
            stop_done = true;
        }
        stop_cv.notify_all();
    });
    for (int i = 0; i < 2000 && service.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(!service.is_running());

    {
        std::lock_guard<std::mutex> lock(hook_mu);
        release_waiter = true;
    }
    hook_cv.notify_all();
    release_resize_block();

    bool stopped_promptly = false;
    {
        std::unique_lock<std::mutex> lock(stop_mu);
        stopped_promptly = stop_cv.wait_for(lock, std::chrono::milliseconds(200),
                                            [&] { return stop_done; });
    }
    waiter.join();
    stopper.join();
    assert(stopped_promptly);
    assert(!done);
    assert(waited.rc == DSP_SVC_ERR_UNAVAILABLE);
}

void test_actual_pool_bytes_are_accounted(HalDspOps* dsp,
                                          HalFrameBufferOps* fb) {
    constexpr uint64_t logical_nv12_bytes = 32U * 32U * 3U / 2U;
    constexpr uint64_t extra_bytes = 512;
    constexpr uint64_t retained_pool_bytes =
        (logical_nv12_bytes + extra_bytes) * 32U;
    g_plane_extra_bytes.store(extra_bytes);

    DspServiceConfig limited_cfg;
    limited_cfg.pool_retention_ms = 0;
    limited_cfg.max_client_pixels = retained_pool_bytes - 1;
    limited_cfg.max_total_buffer_pixels = retained_pool_bytes - 1;
    DspService limited(dsp, fb, limited_cfg);
    assert(limited.start());
    auto rejected = limited.alloc_buffers(231, 32, 32, HAL_PIX_FMT_NV12, 1);
    assert(rejected.rc == DSP_SVC_ERR_LIMIT);
    limited.stop();

    DspServiceConfig exact_cfg;
    exact_cfg.pool_retention_ms = 0;
    exact_cfg.max_client_pixels = retained_pool_bytes;
    exact_cfg.max_total_buffer_pixels = retained_pool_bytes;
    DspService exact(dsp, fb, exact_cfg);
    assert(exact.start());
    auto first = exact.alloc_buffers(232, 32, 32, HAL_PIX_FMT_NV12, 1);
    assert(first.rc == DSP_SVC_OK);
    assert(g_last_pool_max_buffers.load() == 32);
    assert(exact.retained_buffer_bytes_for_test() == retained_pool_bytes);

    auto second = exact.alloc_buffers(232, 32, 32, HAL_PIX_FMT_NV12, 1);
    assert(second.rc == DSP_SVC_OK);
    assert(exact.retained_buffer_bytes_for_test() == retained_pool_bytes);
    assert(exact.release_buffer(232, first.ids[0]) == DSP_SVC_OK);
    assert(exact.retained_buffer_bytes_for_test() == retained_pool_bytes);
    assert(exact.release_buffer(232, second.ids[0]) == DSP_SVC_OK);
    assert(exact.retained_buffer_bytes_for_test() == 0);
    exact.stop();

    g_plane_extra_bytes.store(0);
}

void test_pool_admission_precedes_hal_allocation(HalDspOps* dsp,
                                                 HalFrameBufferOps* fb) {
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 0;
    cfg.max_client_pixels = 256U * 1024U * 1024U;
    cfg.max_total_buffer_pixels = 256U * 1024U * 1024U;
    DspService service(dsp, fb, cfg);
    assert(service.start());

    const int requests_before = g_buffer_requests.load();
    auto rejected = service.alloc_buffers(241, 3840, 2160,
                                          HAL_PIX_FMT_NV12, 1);
    assert(rejected.rc == DSP_SVC_ERR_LIMIT);
    assert(g_buffer_requests.load() == requests_before);
    service.stop();
}

void test_default_caps_allow_common_nv12_pools(HalDspOps* dsp,
                                                HalFrameBufferOps* fb) {
    DspService service(dsp, fb);
    assert(service.start());

    auto full_hd = service.alloc_buffers(246, 1920, 1080,
                                         HAL_PIX_FMT_NV12, 1);
    auto ultra_hd = service.alloc_buffers(246, 3840, 2160,
                                          HAL_PIX_FMT_NV12, 1);
    assert(full_hd.rc == DSP_SVC_OK && ultra_hd.rc == DSP_SVC_OK);
    assert(service.release_buffer(246, full_hd.ids[0]) == DSP_SVC_OK);
    assert(service.release_buffer(246, ultra_hd.ids[0]) == DSP_SVC_OK);

    auto rgb24 = service.alloc_buffers(246, 3840, 2160,
                                       HAL_PIX_FMT_RGB24, 1);
    assert(rgb24.rc == DSP_SVC_OK);
    assert(rgb24.strides[0] == 3840 * 3);
    assert(service.release_buffer(246, rgb24.ids[0]) == DSP_SVC_OK);

    auto argb32 = service.alloc_buffers(246, 3840, 2160,
                                        HAL_PIX_FMT_ARGB32, 1);
    assert(argb32.rc == DSP_SVC_OK);
    assert(argb32.strides[0] == 3840 * 4);
    assert(service.release_buffer(246, argb32.ids[0]) == DSP_SVC_OK);
    service.stop();
}

void test_async_cap_survives_reconnect(HalDspOps* dsp,
                                       HalFrameBufferOps* fb) {
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 0;
    cfg.max_async_jobs_per_client = 1;
    cfg.quota_jobs_per_sec = 1000;
    cfg.quota_mpix_per_sec = 1000;
    DspService service(dsp, fb, cfg);
    assert(service.start());

    int old_conn[2] = {-1, -1};
    int new_conn[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, old_conn) == 0);
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, new_conn) == 0);
    const auto old_desc = allocate_resize_job(service, old_conn[0]);
    const auto new_desc = allocate_resize_job(service, new_conn[0]);

    std::mutex hook_mu;
    std::condition_variable hook_cv;
    bool first_registered = false;
    bool release_first = false;
    uint32_t hook_calls = 0;
    service.set_after_async_register_hook([&] {
        std::unique_lock<std::mutex> lock(hook_mu);
        if (++hook_calls != 1) return;
        first_registered = true;
        hook_cv.notify_all();
        hook_cv.wait(lock, [&] { return release_first; });
    });

    uint64_t old_job_id = 0;
    DspJobResult old_result;
    std::thread old_submitter([&] {
        old_result = service.submit_job_async(old_desc, old_job_id);
    });
    {
        std::unique_lock<std::mutex> lock(hook_mu);
        assert(hook_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return first_registered;
        }));
    }

    service.release_client_buffers(old_conn[0]);
    uint64_t blocked_id = 0;
    const auto blocked = service.submit_job_async(new_desc, blocked_id);
    assert(blocked.rc == DSP_SVC_ERR_QUOTA);
    assert(blocked_id == 0);

    {
        std::lock_guard<std::mutex> lock(hook_mu);
        release_first = true;
    }
    hook_cv.notify_all();
    old_submitter.join();
    assert(old_result.rc == DSP_SVC_ERR_NO_BUFFER);
    assert(old_job_id == 0);

    uint64_t accepted_id = 0;
    const auto accepted = service.submit_job_async(new_desc, accepted_id);
    assert(accepted.rc == DSP_SVC_OK);
    bool done = false;
    const auto completed = service.wait_job(accepted_id, 2000, done);
    assert(done && completed.rc == DSP_SVC_OK);

    service.release_client_buffers(new_conn[0]);
    service.stop();
    for (int fd : old_conn) ::close(fd);
    for (int fd : new_conn) ::close(fd);
}

void test_disconnected_queued_waiter_reports_failure(HalDspOps* dsp,
                                                     HalFrameBufferOps* fb) {
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 0;
    cfg.quota_jobs_per_sec = 1000;
    cfg.quota_mpix_per_sec = 1000;
    cfg.quota_total_jobs_per_sec = 1000;
    cfg.quota_total_mpix_per_sec = 1000;
    DspService service(dsp, fb, cfg);
    assert(service.start());

    arm_resize_block();
    const auto blocker_desc = allocate_resize_job(service, 253);
    uint64_t blocker_id = 0;
    const auto blocker = service.submit_job_async(blocker_desc, blocker_id);
    assert(blocker.rc == DSP_SVC_OK && blocker_id != 0);
    wait_for_resize_entry();

    const auto queued_desc = allocate_resize_job(service, 254);
    uint64_t queued_id = 0;
    const auto queued = service.submit_job_async(queued_desc, queued_id);
    assert(queued.rc == DSP_SVC_OK && queued_id != 0);

    std::mutex hook_mu;
    std::condition_variable hook_cv;
    bool waiter_has_job = false;
    service.set_after_wait_job_lookup_hook([&] {
        std::lock_guard<std::mutex> lock(hook_mu);
        waiter_has_job = true;
        hook_cv.notify_all();
    });
    bool done = false;
    DspJobResult waited;
    std::thread waiter([&] { waited = service.wait_job(queued_id, 2000, done); });
    {
        std::unique_lock<std::mutex> lock(hook_mu);
        assert(hook_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return waiter_has_job;
        }));
    }

    service.release_client_buffers(254);
    waiter.join();
    assert(done);
    assert(waited.rc == DSP_SVC_ERR_UNAVAILABLE);

    service.set_after_wait_job_lookup_hook({});
    release_resize_block();
    done = false;
    const auto blocker_done = service.wait_job(blocker_id, 2000, done);
    assert(done && blocker_done.rc == DSP_SVC_OK);

    service.release_client_buffers(253);
    service.stop();
}

void test_disconnected_running_waiter_reports_failure(HalDspOps* dsp,
                                                      HalFrameBufferOps* fb) {
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 0;
    cfg.quota_jobs_per_sec = 1000;
    cfg.quota_mpix_per_sec = 1000;
    cfg.quota_total_jobs_per_sec = 1000;
    cfg.quota_total_mpix_per_sec = 1000;
    DspService service(dsp, fb, cfg);
    assert(service.start());

    arm_resize_block();
    const auto running_desc = allocate_resize_job(service, 255);
    uint64_t running_id = 0;
    const auto running = service.submit_job_async(running_desc, running_id);
    assert(running.rc == DSP_SVC_OK && running_id != 0);
    wait_for_resize_entry();

    std::mutex hook_mu;
    std::condition_variable hook_cv;
    bool waiter_has_job = false;
    service.set_after_wait_job_lookup_hook([&] {
        std::lock_guard<std::mutex> lock(hook_mu);
        waiter_has_job = true;
        hook_cv.notify_all();
    });
    bool done = false;
    DspJobResult waited;
    std::thread waiter([&] { waited = service.wait_job(running_id, 2000, done); });
    {
        std::unique_lock<std::mutex> lock(hook_mu);
        assert(hook_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return waiter_has_job;
        }));
    }

    service.release_client_buffers(255);
    release_resize_block();
    waiter.join();
    assert(done);
    assert(waited.rc == DSP_SVC_ERR_UNAVAILABLE);
    service.stop();
}

void test_service_async_cap_limits_all_owners(HalDspOps* dsp,
                                               HalFrameBufferOps* fb) {
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 0;
    cfg.max_async_jobs_per_client = 2;
    cfg.max_total_async_jobs = 1;
    cfg.quota_jobs_per_sec = 1000;
    cfg.quota_mpix_per_sec = 1000;
    cfg.quota_total_jobs_per_sec = 1000;
    cfg.quota_total_mpix_per_sec = 1000;
    DspService service(dsp, fb, cfg);
    assert(service.start());

    const auto first_desc = allocate_resize_job(service, 247);
    const auto second_desc = allocate_resize_job(service, 248);

    std::mutex hook_mu;
    std::condition_variable hook_cv;
    bool first_registered = false;
    bool release_first = false;
    uint32_t hook_calls = 0;
    service.set_after_async_register_hook([&] {
        std::unique_lock<std::mutex> lock(hook_mu);
        if (++hook_calls != 1) return;
        first_registered = true;
        hook_cv.notify_all();
        hook_cv.wait(lock, [&] { return release_first; });
    });

    uint64_t first_job_id = 0;
    DspJobResult first_result;
    std::thread first_submitter([&] {
        first_result = service.submit_job_async(first_desc, first_job_id);
    });
    {
        std::unique_lock<std::mutex> lock(hook_mu);
        assert(hook_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return first_registered;
        }));
    }

    uint64_t rejected_id = 0;
    const auto rejected = service.submit_job_async(second_desc, rejected_id);
    assert(rejected.rc == DSP_SVC_ERR_QUOTA);
    assert(rejected.message.find("service") != std::string::npos);
    assert(rejected_id == 0);

    {
        std::lock_guard<std::mutex> lock(hook_mu);
        release_first = true;
    }
    hook_cv.notify_all();
    first_submitter.join();
    assert(first_result.rc == DSP_SVC_OK);
    assert(first_job_id != 0);

    bool done = false;
    const auto completed = service.wait_job(first_job_id, 2000, done);
    assert(done && completed.rc == DSP_SVC_OK);

    uint64_t accepted_id = 0;
    const auto accepted = service.submit_job_async(second_desc, accepted_id);
    assert(accepted.rc == DSP_SVC_OK);
    done = false;
    const auto second_completed = service.wait_job(accepted_id, 2000, done);
    assert(done && second_completed.rc == DSP_SVC_OK);

    service.release_client_buffers(247);
    service.release_client_buffers(248);
    service.stop();
}

void test_service_quota_limits_all_owners(HalDspOps* dsp,
                                           HalFrameBufferOps* fb) {
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 0;
    cfg.max_async_jobs_per_client = 2;
    cfg.max_total_async_jobs = 4;
    cfg.quota_jobs_per_sec = 1000;
    cfg.quota_mpix_per_sec = 1000;
    cfg.quota_total_jobs_per_sec = 1;
    cfg.quota_total_mpix_per_sec = 1000;
    DspService service(dsp, fb, cfg);
    assert(service.start());

    const auto first_desc = allocate_resize_job(service, 249);
    uint64_t first_id = 0;
    const auto first = service.submit_job_async(first_desc, first_id);
    assert(first.rc == DSP_SVC_OK && first_id != 0);
    bool done = false;
    const auto completed = service.wait_job(first_id, 2000, done);
    assert(done && completed.rc == DSP_SVC_OK);

    const auto second_desc = allocate_resize_job(service, 250);
    uint64_t second_id = 0;
    const auto second = service.submit_job_async(second_desc, second_id);
    assert(second.rc == DSP_SVC_ERR_QUOTA);
    assert(second.message.find("service jobs/s") != std::string::npos);
    assert(second_id == 0);

    service.release_client_buffers(249);
    service.release_client_buffers(250);
    service.stop();
}

void test_service_mpix_quota_limits_all_owners(HalDspOps* dsp,
                                                HalFrameBufferOps* fb) {
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 0;
    cfg.max_async_jobs_per_client = 2;
    cfg.max_total_async_jobs = 4;
    cfg.quota_jobs_per_sec = 1000;
    cfg.quota_mpix_per_sec = 1000;
    cfg.quota_total_jobs_per_sec = 1000;
    cfg.quota_total_mpix_per_sec = 0.006;
    DspService service(dsp, fb, cfg);
    assert(service.start());

    const auto first_desc = allocate_resize_job(service, 256);
    uint64_t first_id = 0;
    const auto first = service.submit_job_async(first_desc, first_id);
    assert(first.rc == DSP_SVC_OK && first_id != 0);
    bool done = false;
    const auto completed = service.wait_job(first_id, 2000, done);
    assert(done && completed.rc == DSP_SVC_OK);

    const auto second_desc = allocate_resize_job(service, 257);
    uint64_t second_id = 0;
    const auto second = service.submit_job_async(second_desc, second_id);
    assert(second.rc == DSP_SVC_ERR_QUOTA);
    assert(second.message.find("service MPix/s") != std::string::npos);
    assert(second_id == 0);

    service.release_client_buffers(256);
    service.release_client_buffers(257);
    service.stop();
}

void test_job_rejects_cross_registration_buffers(HalDspOps* dsp,
                                                 HalFrameBufferOps* fb) {
    DspServiceConfig cfg;
    cfg.pool_retention_ms = 0;
    cfg.quota_jobs_per_sec = 1000;
    cfg.quota_mpix_per_sec = 1000;
    DspService service(dsp, fb, cfg);
    assert(service.start());

    auto src = service.alloc_buffers(251, 64, 64, HAL_PIX_FMT_NV12, 1);
    auto dst = service.alloc_buffers(252, 32, 32, HAL_PIX_FMT_NV12, 1);
    assert(src.rc == DSP_SVC_OK && dst.rc == DSP_SVC_OK);

    DspJobDesc desc;
    desc.op = HAL_DSP_OP_RESIZE;
    desc.src_id = src.ids[0];
    desc.dst_ids.push_back(dst.ids[0]);
    uint64_t job_id = 0;
    const auto rejected = service.submit_job_async(desc, job_id);
    assert(rejected.rc == DSP_SVC_ERR_NO_BUFFER);
    assert(job_id == 0);

    service.release_client_buffers(251);
    service.release_client_buffers(252);
    service.stop();
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
    cfg.pool_retention_ms = 0;
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

    // This disconnect occurs after registration but before enqueue. The job
    // must become unqueueable, while its outstanding slot remains charged
    // until the blocked submitter observes the disconnect and rolls back.
    service.release_client_buffers(owner);
    assert(service.async_job_count_for_test() == 0);
    assert(service.client_async_job_count_for_test(owner) == 1);
    assert(g_releases.load() == 0);

    {
        std::lock_guard<std::mutex> lk(hook_mu);
        allow_enqueue = true;
    }
    hook_cv.notify_all();
    const int resizes_before = g_resizes.load();
    submitter.join();
    assert(submit_result.rc == DSP_SVC_ERR_NO_BUFFER);
    assert(job_id == 0);

    for (int i = 0; i < 200 && g_releases.load() != 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(g_releases.load() == 2);
    assert(g_resizes.load() == resizes_before);
    assert(service.async_job_count_for_test() == 0);
    assert(service.client_async_job_count_for_test(owner) == 0);

    service.stop();

    test_stop_fences_async_submission(&dsp, &fb, cfg, false);
    test_stop_fences_async_submission(&dsp, &fb, cfg, true);
    test_buffer_resource_caps(&dsp, &fb);
    test_allocation_reservation_rollback(&dsp, &fb);
    test_import_resource_caps(&dsp, &fb);
    test_import_reservation_rollback(&dsp, &fb);
    test_detached_pins_remain_charged(&dsp, &fb);
    test_stop_waits_for_buffer_pin(&dsp, &fb);
    test_buffer_registration_fenced_by_disconnect(&dsp, &fb);
    test_buffer_registration_fenced_by_stop(&dsp, &fb);
    test_import_registration_fenced_by_disconnect(&dsp, &fb);
    test_pin_bookkeeping_allocation_failures(&dsp, &fb);
    test_random_id_failure_is_transactional(&dsp, &fb);
    test_async_waiters_are_bounded(&dsp, &fb);
    test_stop_wakes_waiters_for_queued_jobs(&dsp, &fb);
    test_actual_pool_bytes_are_accounted(&dsp, &fb);
    test_pool_admission_precedes_hal_allocation(&dsp, &fb);
    test_default_caps_allow_common_nv12_pools(&dsp, &fb);
    test_async_cap_survives_reconnect(&dsp, &fb);
    test_disconnected_queued_waiter_reports_failure(&dsp, &fb);
    test_disconnected_running_waiter_reports_failure(&dsp, &fb);
    test_service_async_cap_limits_all_owners(&dsp, &fb);
    test_service_quota_limits_all_owners(&dsp, &fb);
    test_service_mpix_quota_limits_all_owners(&dsp, &fb);
    test_job_rejects_cross_registration_buffers(&dsp, &fb);

    DspServiceConfig quota_cfg;
    quota_cfg.pool_retention_ms = 0;
    quota_cfg.max_async_jobs_per_client = 2;
    quota_cfg.quota_jobs_per_sec = 1;
    quota_cfg.quota_mpix_per_sec = 1000;

    DspService quota_service(&dsp, &fb, quota_cfg);
    assert(quota_service.start());

    int conn_a[2] = {-1, -1};
    int conn_b[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, conn_a) == 0);
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, conn_b) == 0);

    // Tagged keys must keep the process and legacy-fd namespaces separate,
    // even for values that collided under the former -pid encoding. A new
    // start time for the same PID is a distinct process incarnation.
    assert(quota_service.quota_try_consume_process_for_test(1, 100));
    assert(quota_service.quota_try_consume_legacy_fd_for_test(-1));
    assert(!quota_service.quota_try_consume_process_for_test(1, 100));
    assert(quota_service.quota_try_consume_process_for_test(1, 101));

    auto process_job_a = allocate_resize_job(quota_service, conn_a[0]);
    uint64_t process_job_id = 0;
    auto process_first = quota_service.submit_job_async(process_job_a,
                                                         process_job_id);
    assert(process_first.rc == DSP_SVC_OK);
    quota_service.release_client_buffers(conn_a[0]);

    auto process_job_b = allocate_resize_job(quota_service, conn_b[0]);
    process_job_id = 0;
    auto process_second = quota_service.submit_job_async(process_job_b,
                                                          process_job_id);
    assert(process_second.rc == DSP_SVC_ERR_QUOTA);
    assert(process_second.message.find("jobs/s") != std::string::npos);
    quota_service.release_client_buffers(conn_b[0]);

    int fallback_pipe[2] = {-1, -1};
    assert(::pipe(fallback_pipe) == 0);
    auto fallback_job = allocate_resize_job(quota_service, fallback_pipe[0]);
    uint64_t fallback_job_id = 0;
    auto fallback_first = quota_service.submit_job_async(fallback_job,
                                                          fallback_job_id);
    assert(fallback_first.rc == DSP_SVC_OK);
    quota_service.release_client_buffers(fallback_pipe[0]);

    int fallback_pipe_second[2] = {-1, -1};
    assert(::pipe(fallback_pipe_second) == 0);
    fallback_job =
        allocate_resize_job(quota_service, fallback_pipe_second[0]);
    fallback_job_id = 0;
    auto fallback_second = quota_service.submit_job_async(fallback_job,
                                                           fallback_job_id);
    assert(fallback_second.rc == DSP_SVC_OK);
    quota_service.release_client_buffers(fallback_pipe_second[0]);

    quota_service.stop();
    for (int fd : conn_a) ::close(fd);
    for (int fd : conn_b) ::close(fd);
    for (int fd : fallback_pipe) ::close(fd);
    for (int fd : fallback_pipe_second) ::close(fd);

    std::puts("dsp_service_async_lifecycle_test: PASS");
    return 0;
}
